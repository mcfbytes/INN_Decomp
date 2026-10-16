/* Host audio: OPL register access at the native clock, or the device in a hybrid-hw build. */
#include "audio.h"
#include "host_audio.h"
#include "lift_semantic.h"
#include "native.h"

void host_audio_opl_write(Machine *m, uint8_t reg, uint8_t value)
{
    if (m->native) {
        native_safepoint(m);
        opl_write_reg(m, reg, value);
        return;
    }
    /* Shadow replay: the original's writes already reached the chip. Record the meaning only. */
    if (m->io_mode == IO_REPLAY) {
        lift_semantic_event(m, SEM_OPL_WRITE, reg, value);
        return;
    }
    opl_write_reg(m, reg, value);
}

uint8_t host_audio_opl_status(Machine *m)
{
    if (m->native)
        native_safepoint(m);
    return opl_read_status(m); /* delay reads are dropped from the semantic comparison */
}

int host_audio_pcm_start(Machine *m, uint32_t linear, uint16_t bytes, uint32_t rate_hz, bool auto_init)
{
    (void)m;
    (void)linear;
    (void)bytes;
    (void)rate_hz;
    (void)auto_init;
    fatal("not implemented");
}

void host_audio_pcm_stop(Machine *m)
{
    (void)m;
    fatal("not implemented");
}
