/* SDL2 audio sink in queue mode rather than a callback: the main loop pushes what the mixer
 * rendered, so there is no lock and no audio thread (the browser build has neither). */
#include <SDL.h>
#include <stdio.h>

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
    WavWriter *wav;
    int16_t buf[PUMP_CAP_FRAMES * AUDIO_CHANNELS];
} AudioSinks;

static AudioSinks g_sinks; /* one machine per process; Emscripten callbacks take no context */

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
#ifdef __EMSCRIPTEN__
    audio_set_target_ms(m, WEB_TARGET_MS); /* the output runs on the busy main thread */
#endif
    SDL_PauseAudioDevice(g_sinks.device, 0);
    return 0;
}

int frontend_audio_open(Machine *m, bool device, const char *wav_path)
{
    int rc = 0;
    if (device && open_device(m) != 0) {
        fprintf(stderr, "audio: no output device (%s); sound is off\n", SDL_GetError());
        rc = -1;
    }
    if (wav_path && !(g_sinks.wav = wav_open(wav_path, audio_rate(m)))) {
        fprintf(stderr, "audio: cannot write %s\n", wav_path);
        rc = -1;
    }
    audio_enable(m, g_sinks.device || g_sinks.wav);
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
    if (g_sinks.device) {
        SDL_CloseAudioDevice(g_sinks.device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_sinks.device = 0;
    }
    audio_enable(m, false);
    return rc;
}
