/* The CON device: keyboard input through the BIOS keyboard buffer (blocking calls retry until a
 * key arrives) and console output, which goes to the host log one line at a time. */
#define _POSIX_C_SOURCE 200809L /* fds, directories, localtime_r on any libc */
#include <string.h>

#include "devices.h"
#include "dos_int.h"

enum { KEY_CR = '\r', KEY_LF = '\n', KEY_BS = 0x08, KEY_EXT_PREFIX = 0xE0, DIRECT_INPUT = 0xFF };
enum { BUFIN_MAX = 0, BUFIN_COUNT = 1, BUFIN_TEXT = 2 };

/* ---- output ---- */

static void flush_line(DosState *d)
{
    d->out_line[d->out_len] = 0;
    log_msg("dos stdout: %s", d->out_line);
    d->out_len = 0;
}

void dos_con_write(Machine *m, const uint8_t *buf, size_t len)
{
    DosState *d = dos_state(m);
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = buf[i];
        if (ch == KEY_LF)
            flush_line(d);
        else if (ch != KEY_CR)
            d->out_line[d->out_len++] = ch < ' ' ? '.' : (char)ch;
        if (d->out_len == (int)sizeof d->out_line - 1)
            flush_line(d);
    }
}

static void con_putc(Machine *m, uint8_t ch) { dos_con_write(m, &ch, 1); }

/* ---- input ---- */

/* Next character, DOS style: an extended key arrives as 0 followed by its scan code. */
static bool con_getc(Machine *m, uint8_t *ch)
{
    DosState *d = dos_state(m);
    if (d->pending_scan) {
        *ch = d->pending_scan;
        d->pending_scan = 0;
        return true;
    }
    uint16_t key;
    if (!bios_kbd_pop(m, &key))
        return false;
    uint8_t ascii = (uint8_t)key, scan = (uint8_t)(key >> 8);
    if (ascii == 0 || (ascii == KEY_EXT_PREFIX && scan)) {
        d->pending_scan = scan;
        *ch = 0;
        return true;
    }
    *ch = ascii;
    return true;
}

bool dos_con_key_ready(Machine *m)
{
    uint16_t key;
    return dos_state(m)->pending_scan || bios_kbd_peek(m, &key);
}

/* Collects keys into the line buffer with echo; true once Enter was pressed. */
static bool edit_line(Machine *m, int max_chars)
{
    DosState *d = dos_state(m);
    uint8_t ch;
    while (con_getc(m, &ch)) {
        if (ch == KEY_CR) {
            con_putc(m, KEY_LF);
            return true;
        }
        if (ch == 0) {
            d->pending_scan = 0; /* editing keys are ignored */
        } else if (ch == KEY_BS) {
            if (d->in_len > 0)
                d->in_len--;
        } else if (d->in_len < max_chars && d->in_len < (int)sizeof d->in_line) {
            d->in_line[d->in_len++] = (char)ch;
            con_putc(m, ch);
        }
    }
    return false;
}

bool dos_con_read(Machine *m, uint8_t *buf, uint16_t cap, uint16_t *got)
{
    DosState *d = dos_state(m);
    if (d->ready_pos >= d->ready_len) {
        if (!edit_line(m, (int)sizeof d->in_line - 2))
            return false;
        memcpy(d->in_ready, d->in_line, (size_t)d->in_len);
        d->in_ready[d->in_len] = KEY_CR;
        d->in_ready[d->in_len + 1] = KEY_LF;
        d->ready_len = d->in_len + 2;
        d->ready_pos = d->in_len = 0;
    }
    uint16_t n = (uint16_t)(d->ready_len - d->ready_pos);
    if (n > cap)
        n = cap;
    memcpy(buf, d->in_ready + d->ready_pos, n);
    d->ready_pos += n;
    *got = n;
    return true;
}

/* ---- INT 21h character functions ---- */

static CallbackResult read_char(Machine *m, bool echo)
{
    uint8_t ch;
    if (!con_getc(m, &ch))
        return CB_RETRY;
    if (echo && ch)
        con_putc(m, ch);
    AL = ch;
    return CB_DONE;
}

CallbackResult dos_con_in_echo(Machine *m) { return read_char(m, true); }
CallbackResult dos_con_in_raw(Machine *m) { return read_char(m, false); }
CallbackResult dos_con_in(Machine *m) { return read_char(m, false); }

CallbackResult dos_con_out(Machine *m)
{
    con_putc(m, DL);
    AL = DL;
    return CB_DONE;
}

CallbackResult dos_aux_in(Machine *m)
{
    AL = 0;
    return CB_DONE;
}

CallbackResult dos_aux_out(Machine *m) { return CB_DONE; }
CallbackResult dos_prn_out(Machine *m) { return CB_DONE; }

CallbackResult dos_direct_con(Machine *m)
{
    if (DL != DIRECT_INPUT) {
        con_putc(m, DL);
        AL = DL;
        return CB_DONE;
    }
    uint8_t ch = 0;
    bool got = con_getc(m, &ch);
    AL = ch;
    cb_set_zero(m, !got);
    return CB_DONE;
}

CallbackResult dos_print_string(Machine *m)
{
    for (uint16_t off = DX;; off++) {
        uint8_t ch = mem_rd8(m, real_addr(DS, off));
        if (ch == '$')
            break;
        con_putc(m, ch);
        if (off == 0xFFFF)
            break;
    }
    AL = '$';
    return CB_DONE;
}

CallbackResult dos_buffered_input(Machine *m)
{
    DosState *d = dos_state(m);
    uint32_t buf = real_addr(DS, DX);
    uint8_t max = mem_rd8(m, buf + BUFIN_MAX);
    if (max == 0)
        return CB_DONE;
    if (!edit_line(m, max - 1))
        return CB_RETRY;
    mem_wr8(m, buf + BUFIN_COUNT, (uint8_t)d->in_len);
    mem_write_block(m, buf + BUFIN_TEXT, d->in_line, (size_t)d->in_len);
    mem_wr8(m, buf + BUFIN_TEXT + (uint32_t)d->in_len, KEY_CR);
    d->in_len = 0;
    return CB_DONE;
}

CallbackResult dos_con_status(Machine *m)
{
    AL = dos_con_key_ready(m) ? 0xFF : 0;
    if (!AL)
        machine_idle_poll(m, IDLE_POLL_CONSOLE, 0);
    return CB_DONE;
}

static CallbackResult input_function(Machine *m, uint8_t fn)
{
    switch (fn) {
    case 0x01: return dos_con_in_echo(m);
    case 0x06: return dos_direct_con(m);
    case 0x07: return dos_con_in_raw(m);
    case 0x08: return dos_con_in(m);
    case 0x0A: return dos_buffered_input(m);
    }
    return CB_DONE;
}

/* AH=0Ch: empty the type-ahead buffer, then run input function AL. A retry while that function
 * waits must not flush again, or the awaited key would be thrown away. */
CallbackResult dos_con_flush(Machine *m)
{
    DosState *d = dos_state(m);
    uint16_t key;
    if (!d->flush_done) {
        while (bios_kbd_pop(m, &key))
            ;
        d->pending_scan = 0;
        d->flush_done = true;
    }
    CallbackResult r = input_function(m, AL);
    if (r == CB_DONE)
        d->flush_done = false;
    return r;
}

CallbackResult dos_int29(Machine *m)
{
    con_putc(m, AL);
    return CB_DONE;
}
