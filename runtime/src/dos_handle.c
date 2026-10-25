/* The system file table (host side) and each process's job file table (in its PSP, or in a block
 * allocated by AH=67h): handle numbers, inheritance, dup, IOCTL. */
#define _POSIX_C_SOURCE 200809L /* fds, directories, localtime_r on any libc */
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dos_int.h"

enum { SFT_CON, SFT_AUX, SFT_PRN, SFT_FIRST_FILE };
enum { OPEN_NO_INHERIT = 0x80 };

void dos_files_init(Machine *m)
{
    DosState *d = dos_state(m);
    for (int i = 0; i < DOS_SFT_MAX; i++)
        d->sft[i].fd = -1;
    const DosDevice std_devices[] = {DEV_CON, DEV_AUX, DEV_PRN};
    const char *std_names[] = {"CON", "AUX", "PRN"};
    for (int i = 0; i < SFT_FIRST_FILE; i++) {
        d->sft[i].dev = std_devices[i];
        d->sft[i].mode = 2; /* read/write */
        strcpy(d->sft[i].dos_path, std_names[i]);
    }
    /* Guest-visible SFT header: an empty table, so walkers stop at once. */
    uint32_t hdr = real_addr(DOS_DATA_SEG, DD_SFT);
    mem_wr32(m, hdr, 0xFFFFFFFFu);
    mem_wr16(m, hdr + 4, 0);
}

void dos_files_shutdown(Machine *m)
{
    DosState *d = dos_state(m);
    for (int i = 0; i < DOS_SFT_MAX; i++)
        if (d->sft[i].fd >= 0) {
            close(d->sft[i].fd);
            d->sft[i].fd = -1;
        }
}

static uint32_t jft_addr(Machine *m, uint16_t psp)
{
    uint32_t p = real_addr(psp, PSP_JFT_PTR);
    return real_addr(mem_rd16(m, p + 2), mem_rd16(m, p));
}

uint16_t dos_jft_size(Machine *m, uint16_t psp) { return mem_rd16(m, real_addr(psp, PSP_JFT_SIZE)); }

int dos_jft_get(Machine *m, uint16_t psp, uint16_t handle)
{
    if (handle >= dos_jft_size(m, psp))
        return -1;
    uint8_t v = mem_rd8(m, jft_addr(m, psp) + handle);
    return v == JFT_CLOSED || v >= DOS_SFT_MAX ? -1 : v;
}

void dos_jft_set(Machine *m, uint16_t psp, uint16_t handle, uint8_t sft)
{
    mem_wr8(m, jft_addr(m, psp) + handle, sft);
}

DosFile *dos_handle_file(Machine *m, uint16_t handle)
{
    DosState *d = dos_state(m);
    int idx = dos_jft_get(m, d->cur_psp, handle);
    if (idx < 0 || d->sft[idx].refs <= 0)
        return NULL;
    return &d->sft[idx];
}

int dos_sft_alloc(Machine *m)
{
    DosState *d = dos_state(m);
    for (int i = SFT_FIRST_FILE; i < DOS_SFT_MAX; i++)
        if (d->sft[i].refs == 0 && d->sft[i].fd < 0)
            return i;
    return -1;
}

void dos_sft_release(Machine *m, int idx)
{
    DosFile *f = &dos_state(m)->sft[idx];
    if (f->refs > 0 && --f->refs == 0 && f->fd >= 0) {
        dos_host_close(f->fd);
        f->fd = -1;
        if (f->written)
            dos_host_file_closed(f->dos_path);
        f->written = false;
    }
}

int dos_alloc_handle(Machine *m, int sft_idx)
{
    DosState *d = dos_state(m);
    uint16_t size = dos_jft_size(m, d->cur_psp);
    uint32_t jft = jft_addr(m, d->cur_psp);
    for (uint16_t h = 0; h < size; h++)
        if (mem_rd8(m, jft + h) == JFT_CLOSED) {
            mem_wr8(m, jft + h, (uint8_t)sft_idx);
            d->sft[sft_idx].refs++;
            return h;
        }
    return -1;
}

void dos_inherit_handles(Machine *m, uint16_t parent, uint16_t child)
{
    DosState *d = dos_state(m);
    uint16_t size = dos_jft_size(m, parent);
    for (uint16_t h = 0; h < DOS_JFT_DEFAULT; h++) {
        int idx = h < size ? dos_jft_get(m, parent, h) : -1;
        if (idx >= 0 && d->sft[idx].refs > 0 && !(d->sft[idx].mode & OPEN_NO_INHERIT)) {
            dos_jft_set(m, child, h, (uint8_t)idx);
            d->sft[idx].refs++;
        } else {
            dos_jft_set(m, child, h, JFT_CLOSED);
        }
    }
}

void dos_close_all(Machine *m, uint16_t psp)
{
    uint16_t size = dos_jft_size(m, psp);
    for (uint16_t h = 0; h < size; h++) {
        int idx = dos_jft_get(m, psp, h);
        if (idx >= 0)
            dos_sft_release(m, idx);
        dos_jft_set(m, psp, h, JFT_CLOSED);
    }
}

CallbackResult dos_close(Machine *m)
{
    DosState *d = dos_state(m);
    int idx = dos_jft_get(m, d->cur_psp, BX);
    if (idx < 0 || d->sft[idx].refs <= 0)
        return dos_fail(m, DE_INVALID_HANDLE);
    dos_sft_release(m, idx);
    dos_jft_set(m, d->cur_psp, BX, JFT_CLOSED);
    return dos_ok(m);
}

CallbackResult dos_dup(Machine *m)
{
    DosState *d = dos_state(m);
    int idx = dos_jft_get(m, d->cur_psp, BX);
    if (idx < 0 || d->sft[idx].refs <= 0)
        return dos_fail(m, DE_INVALID_HANDLE);
    int h = dos_alloc_handle(m, idx);
    if (h < 0)
        return dos_fail(m, DE_TOO_MANY_OPEN);
    AX = (uint16_t)h;
    return dos_ok(m);
}

CallbackResult dos_dup2(Machine *m)
{
    DosState *d = dos_state(m);
    int idx = dos_jft_get(m, d->cur_psp, BX);
    if (idx < 0 || d->sft[idx].refs <= 0 || CX >= dos_jft_size(m, d->cur_psp))
        return dos_fail(m, DE_INVALID_HANDLE);
    if (BX == CX)
        return dos_ok(m);
    int old = dos_jft_get(m, d->cur_psp, CX);
    if (old >= 0)
        dos_sft_release(m, old);
    dos_jft_set(m, d->cur_psp, CX, (uint8_t)idx);
    d->sft[idx].refs++;
    return dos_ok(m);
}

/* Grows the JFT into its own memory block when a program wants more than 20 handles. */
CallbackResult dos_set_handle_count(Machine *m)
{
    DosState *d = dos_state(m);
    uint16_t psp = d->cur_psp, size = dos_jft_size(m, psp);
    if (BX <= size)
        return dos_ok(m);
    uint16_t seg, largest;
    int err = dos_mem_alloc(m, (uint16_t)((BX + PARA - 1) / PARA), psp, &seg, &largest);
    if (err)
        return dos_fail(m, (uint16_t)err);
    uint32_t old = jft_addr(m, psp), new_jft = real_addr(seg, 0);
    for (uint16_t h = 0; h < BX; h++)
        mem_wr8(m, new_jft + h, h < size ? mem_rd8(m, old + h) : JFT_CLOSED);
    uint16_t old_seg = mem_rd16(m, real_addr(psp, PSP_JFT_PTR + 2));
    if (old_seg != psp)
        dos_mem_free(m, old_seg);
    mem_wr16(m, real_addr(psp, PSP_JFT_SIZE), BX);
    mem_wr16(m, real_addr(psp, PSP_JFT_PTR), 0);
    mem_wr16(m, real_addr(psp, PSP_JFT_PTR + 2), seg);
    return dos_ok(m);
}

/* ---- IOCTL (AH=44h) ---- */

enum {
    DEVINFO_DEVICE = 0x80, DEVINFO_STDIN = 0x01, DEVINFO_STDOUT = 0x02, DEVINFO_NUL = 0x04,
    DEVINFO_CLOCK = 0x08, DEVINFO_SPECIAL = 0x10, DEVINFO_NOT_EOF = 0x40, DEVINFO_CHAR_HI = 0x8000,
};

static uint16_t device_info(const DosFile *f)
{
    switch (f->dev) {
    case DEV_CON:
        return DEVINFO_CHAR_HI | DEVINFO_DEVICE | DEVINFO_NOT_EOF | DEVINFO_SPECIAL | DEVINFO_STDIN |
               DEVINFO_STDOUT;
    case DEV_NUL: return DEVINFO_CHAR_HI | DEVINFO_DEVICE | DEVINFO_NUL;
    case DEV_CLOCK: return DEVINFO_CHAR_HI | DEVINFO_DEVICE | DEVINFO_CLOCK;
    case DEV_EMM: return DEVINFO_CHAR_HI | DEVINFO_DEVICE;
    case DEV_AUX:
    case DEV_PRN: return DEVINFO_CHAR_HI | DEVINFO_DEVICE | DEVINFO_NOT_EOF;
    case DEV_NONE: break;
    }
    return DOS_DRIVE_C;
}

static CallbackResult ioctl_input_status(Machine *m, DosFile *f)
{
    if (f->dev == DEV_CON) {
        AL = dos_con_key_ready(m) ? 0xFF : 0;
    } else if (f->dev != DEV_NONE) {
        AL = 0;
    } else {
        struct stat st;
        off_t pos = lseek(f->fd, 0, SEEK_CUR);
        AL = fstat(f->fd, &st) == 0 && pos < st.st_size ? 0xFF : 0;
    }
    return dos_ok(m);
}

static CallbackResult ioctl_handle(Machine *m)
{
    DosFile *f = dos_handle_file(m, BX);
    if (!f)
        return dos_fail(m, DE_INVALID_HANDLE);
    switch (AL) {
    case 0x00:
        DX = AX = device_info(f);
        return dos_ok(m);
    case 0x01:
        return dos_ok(m);
    case 0x06:
        return ioctl_input_status(m, f);
    case 0x07:
        AL = 0xFF;
        return dos_ok(m);
    case 0x0A:
        DX = 0;
        return dos_ok(m);
    }
    return dos_fail(m, DE_FUNCTION);
}

static bool ioctl_drive_ok(uint8_t bl) { return bl == 0 || bl == DOS_DRIVE_C + 1; }

CallbackResult dos_ioctl(Machine *m)
{
    switch (AL) {
    case 0x00: case 0x01: case 0x06: case 0x07: case 0x0A:
        return ioctl_handle(m);
    case 0x08: /* removable media */
        if (!ioctl_drive_ok(BL))
            return dos_fail(m, DE_INVALID_DRIVE);
        AX = 1; /* fixed disk */
        return dos_ok(m);
    case 0x09: /* remote drive */
        if (!ioctl_drive_ok(BL))
            return dos_fail(m, DE_INVALID_DRIVE);
        DX = 0;
        return dos_ok(m);
    case 0x0B: /* sharing retry count */
        return dos_ok(m);
    case 0x0E: /* logical drive map: only one letter per drive */
        AL = 0;
        return dos_ok(m);
    }
    if (m->trace)
        log_msg("dos: IOCTL AL=%02X unsupported", AL);
    return dos_fail(m, DE_FUNCTION);
}
