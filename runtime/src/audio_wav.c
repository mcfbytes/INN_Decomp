/* Minimal RIFF/WAVE writer for 16-bit stereo PCM; the sizes are patched in on close. */
#include <stdio.h>
#include <stdlib.h>

#include "audio.h"

enum {
    WAV_HEADER_BYTES = 44, RIFF_SIZE_AT = 4, DATA_SIZE_AT = 40,
    FMT_CHUNK_BYTES = 16, FORMAT_PCM = 1, BITS_PER_SAMPLE = 16,
    FRAME_BYTES = AUDIO_CHANNELS * BITS_PER_SAMPLE / 8,
    WRITE_CHUNK_SAMPLES = 4096,
};

struct WavWriter {
    FILE *f;
    int rate;
    uint32_t data_bytes;
    bool failed;
};

static void put_le(uint8_t *at, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        at[i] = (uint8_t)(v >> (8 * i));
}

static void build_header(uint8_t *h, int rate, uint32_t data_bytes)
{
    const char *tags[] = { "RIFF", "WAVE", "fmt ", "data" };
    const int tag_at[] = { 0, 8, 12, 36 };
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 4; i++)
            h[tag_at[t] + i] = (uint8_t)tags[t][i];
    put_le(h + RIFF_SIZE_AT, WAV_HEADER_BYTES - 8 + data_bytes, 4);
    put_le(h + 16, FMT_CHUNK_BYTES, 4);
    put_le(h + 20, FORMAT_PCM, 2);
    put_le(h + 22, AUDIO_CHANNELS, 2);
    put_le(h + 24, (uint32_t)rate, 4);
    put_le(h + 28, (uint32_t)rate * FRAME_BYTES, 4);
    put_le(h + 32, FRAME_BYTES, 2);
    put_le(h + 34, BITS_PER_SAMPLE, 2);
    put_le(h + DATA_SIZE_AT, data_bytes, 4);
}

WavWriter *wav_open(const char *path, int rate)
{
    WavWriter *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    uint8_t h[WAV_HEADER_BYTES];
    build_header(h, rate, 0);
    w->f = fopen(path, "wb");
    if (!w->f || fwrite(h, 1, sizeof h, w->f) != sizeof h) {
        if (w->f)
            fclose(w->f);
        free(w);
        return NULL;
    }
    w->rate = rate;
    return w;
}

int wav_write(WavWriter *w, const int16_t *frames, int count)
{
    uint8_t buf[WRITE_CHUNK_SAMPLES * 2];
    int total = count * AUDIO_CHANNELS;
    for (int done = 0; done < total && !w->failed;) {
        int n = total - done < WRITE_CHUNK_SAMPLES ? total - done : WRITE_CHUNK_SAMPLES;
        for (int i = 0; i < n; i++)
            put_le(buf + 2 * i, (uint16_t)frames[done + i], 2);
        w->failed = fwrite(buf, 2, (size_t)n, w->f) != (size_t)n;
        done += n;
    }
    if (!w->failed)
        w->data_bytes += (uint32_t)count * FRAME_BYTES;
    return w->failed ? -1 : 0;
}

int wav_close(WavWriter *w)
{
    uint8_t h[WAV_HEADER_BYTES];
    build_header(h, w->rate, w->data_bytes);
    bool ok = !w->failed && fseek(w->f, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof h, w->f) == sizeof h;
    ok = fclose(w->f) == 0 && ok;
    free(w);
    return ok ? 0 : -1;
}
