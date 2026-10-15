/* What the frontend gives the native core (docs/native.md section 9): wall clock, sleeping (a JSPI
 * suspension in the browser), events, presentation and audio. */
#ifndef DOSRT_HOST_PLATFORM_H
#define DOSRT_HOST_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

typedef struct HostPlatform {
    void *ctx;
    uint64_t (*now_us)(void *ctx);                    /* monotonic wall clock */
    void (*sleep_us)(void *ctx, uint64_t us);         /* 0: yield once; browser: JSPI suspension */
    void (*poll_events)(void *ctx, Machine *m);       /* feeds host_input_* */
    void (*present)(void *ctx, const uint32_t *argb, int w, int h);
    void (*audio_pump)(void *ctx, Machine *m);
    bool (*quit_requested)(void *ctx);
} HostPlatform;

void native_set_platform(Machine *m, const HostPlatform *p); /* NULL: headless, deterministic */

#endif
