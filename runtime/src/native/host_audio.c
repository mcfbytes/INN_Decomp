/* Host audio stubs until package 5 implements them. */
#include "host_audio.h"

void host_audio_opl_write(Machine *m, uint8_t reg, uint8_t value) { fatal("host_audio_opl_write: not implemented (package 5)"); }
uint8_t host_audio_opl_status(Machine *m) { fatal("host_audio_opl_status: not implemented (package 5)"); }
int host_audio_pcm_start(Machine *m, uint32_t linear, uint16_t bytes, uint32_t rate_hz, bool auto_init) { fatal("host_audio_pcm_start: not implemented (package 5)"); }
void host_audio_pcm_stop(Machine *m) { fatal("host_audio_pcm_stop: not implemented (package 5)"); }
