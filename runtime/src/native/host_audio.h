/* Host audio (docs/native.md section 9, package 5). The OPL2 core, mixer and sinks are the existing
 * ones; only the register access changes from ports to calls, at the native clock's time. Every call
 * calls native_safepoint first. */
#ifndef DOSRT_HOST_AUDIO_H
#define DOSRT_HOST_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

void host_audio_opl_write(Machine *m, uint8_t reg, uint8_t value);
uint8_t host_audio_opl_status(Machine *m);                 /* timer flags, for the detection probe */
/* Sound Blaster digital output, 8-bit mono from guest memory; raises IRQ 7 at each block end. */
int host_audio_pcm_start(Machine *m, uint32_t linear, uint16_t bytes, uint32_t rate_hz, bool auto_init);
void host_audio_pcm_stop(Machine *m);

#endif
