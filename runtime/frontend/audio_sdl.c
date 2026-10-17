/* Audio sinks. Natively: SDL2 in queue mode rather than a callback, so the main loop pushes what the
 * mixer rendered and no audio thread touches the machine. In the browser: an AudioWorklet
 * (runtime/web/audio-worklet.js) that the main loop posts frames to, so the output runs on the audio
 * thread and a busy main thread no longer starves it (SDL's backend there is a ScriptProcessorNode,
 * which runs on the main thread and is deprecated). */
#include <SDL.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "audio.h"
#include "audio_sdl.h"

enum {
    DEVICE_BUFFER_FRAMES = 1024, /* 21 ms at 48 kHz; a power of two for WebAudio */
    WEB_TARGET_MS = 100,
    FRAME_BYTES = AUDIO_CHANNELS * (int)sizeof(int16_t),
    PUMP_CAP_FRAMES = AUDIO_RING_FRAMES * 2, /* ring contents plus re-prime silence and stretch */
};

typedef struct AudioSinks {
    SDL_AudioDeviceID device;
    bool web; /* the browser's AudioWorklet sink */
    WavWriter *wav;
    int16_t buf[PUMP_CAP_FRAMES * AUDIO_CHANNELS];
} AudioSinks;

static AudioSinks g_sinks; /* one machine per process; Emscripten callbacks take no context */

#ifdef __EMSCRIPTEN__
/* The page finds the context as Module.innAudio.ctx (mute, resume on the first click). Returns the output
 * rate, or 0 without AudioWorklet support. The worklet module loads asynchronously; until it is running,
 * web_audio_queued returns -1 and the frames rendered meanwhile are dropped. */
EM_JS(int, web_audio_open, (int rate), {
    const Ctx = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (!Ctx || typeof AudioWorkletNode === 'undefined') return 0;
    let ctx;
    try { ctx = new Ctx({ sampleRate: rate, latencyHint: 'interactive' }); } catch (e) { ctx = new Ctx(); }
    if (!ctx.audioWorklet) { ctx.close(); return 0; }
    const sink = { ctx: ctx, node: null, sent: 0, taken: 0, at: 0, starved: 0 };
    Module.innAudio = sink;
    const url = typeof innAsset === 'function' ? innAsset('audio-worklet.js') : 'audio-worklet.js';
    ctx.audioWorklet.addModule(url).then(() => {
        const node = new AudioWorkletNode(ctx, 'inn-sink', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
        node.port.onmessage = (e) => { sink.taken = e.data.taken; sink.starved = e.data.starved; sink.at = ctx.currentTime; };
        node.connect(ctx.destination);
        sink.node = node;
    }).catch((e) => console.warn('audio: the worklet did not load (' + e + '); sound is off'));
    return ctx.sampleRate;
});

/* Frames posted and not yet played: the worklet's count at its last report, less what it has played
 * since at the output rate. -1 while the sink cannot take frames (loading, or suspended: muted, or
 * waiting for the first click). */
EM_JS(int, web_audio_queued, (void), {
    const s = Module.innAudio;
    if (!s || !s.node || s.ctx.state !== 'running') return -1;
    const since = Math.max(0, (s.ctx.currentTime - s.at) * s.ctx.sampleRate);
    return Math.max(0, Math.round(s.sent - s.taken - since));
});

EM_JS(void, web_audio_post, (const int16_t *frames, int count), {
    const s = Module.innAudio;
    const data = HEAP16.slice(frames >> 1, (frames >> 1) + count * 2);
    s.node.port.postMessage(data, [data.buffer]);
    s.sent += count;
});

EM_JS(void, web_audio_close, (void), {
    const s = Module.innAudio;
    if (s) { if (s.node) s.node.disconnect(); s.ctx.close(); }
    Module.innAudio = null;
});

static int open_device(Machine *m)
{
    int rate = web_audio_open(AUDIO_DEFAULT_RATE);
    if (!rate)
        return -1;
    g_sinks.web = true;
    audio_set_rate(m, rate);
    audio_set_target_ms(m, WEB_TARGET_MS); /* frames are posted from the busy main thread */
    return 0;
}
#else
static int open_device(Machine *m)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return -1;
    SDL_AudioSpec want = { .freq = AUDIO_DEFAULT_RATE, .format = AUDIO_S16SYS,
                           .channels = AUDIO_CHANNELS, .samples = DEVICE_BUFFER_FRAMES };
    SDL_AudioSpec have;
    g_sinks.device = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_sinks.device)
        return -1;
    audio_set_rate(m, have.freq);
    SDL_PauseAudioDevice(g_sinks.device, 0);
    return 0;
}
#endif

int frontend_audio_open(Machine *m, bool device, const char *wav_path)
{
    int rc = 0;
    if (device && open_device(m) != 0) {
#ifdef __EMSCRIPTEN__
        fprintf(stderr, "audio: this browser has no AudioWorklet; sound is off\n");
#else
        fprintf(stderr, "audio: no output device (%s); sound is off\n", SDL_GetError());
#endif
        rc = -1;
    }
    if (wav_path && !(g_sinks.wav = wav_open(wav_path, audio_rate(m)))) {
        fprintf(stderr, "audio: cannot write %s\n", wav_path);
        rc = -1;
    }
    audio_enable(m, g_sinks.device || g_sinks.web || g_sinks.wav);
    return rc;
}

static void write_wav(const int16_t *frames, int count)
{
    if (g_sinks.wav && wav_write(g_sinks.wav, frames, count) != 0)
        fprintf(stderr, "audio: WAV write failed\n");
}

void frontend_audio_pump(Machine *m)
{
    int n;
#ifdef __EMSCRIPTEN__
    if (g_sinks.web) {
        int queued = web_audio_queued();
        if (queued < 0) { /* not playing: what was rendered meanwhile is not kept to play late */
            while (audio_read(m, g_sinks.buf, PUMP_CAP_FRAMES) > 0) {
            }
            return;
        }
        n = audio_read_paced(m, g_sinks.buf, PUMP_CAP_FRAMES, queued);
        if (n > 0)
            web_audio_post(g_sinks.buf, n);
        write_wav(g_sinks.buf, n);
        return;
    }
#endif
    if (g_sinks.device) {
        int queued = (int)(SDL_GetQueuedAudioSize(g_sinks.device) / FRAME_BYTES);
        n = audio_read_paced(m, g_sinks.buf, PUMP_CAP_FRAMES, queued);
        if (n > 0 && SDL_QueueAudio(g_sinks.device, g_sinks.buf, (Uint32)(n * FRAME_BYTES)) != 0)
            fprintf(stderr, "audio: %s\n", SDL_GetError());
        write_wav(g_sinks.buf, n);
        return;
    }
    while (g_sinks.wav && (n = audio_read(m, g_sinks.buf, PUMP_CAP_FRAMES)) > 0)
        write_wav(g_sinks.buf, n);
}

int frontend_audio_close(Machine *m)
{
    int rc = 0;
    if (g_sinks.wav) {
        frontend_audio_pump(m);
        rc = wav_close(g_sinks.wav);
        g_sinks.wav = NULL;
        if (rc != 0)
            fprintf(stderr, "audio: cannot finish the WAV file\n");
    }
#ifdef __EMSCRIPTEN__
    if (g_sinks.web) {
        web_audio_close();
        g_sinks.web = false;
    }
#endif
    if (g_sinks.device) {
        SDL_CloseAudioDevice(g_sinks.device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_sinks.device = 0;
    }
    audio_enable(m, false);
    return rc;
}
