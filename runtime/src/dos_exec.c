/* Program loading: MZ and .COM images, environment blocks, EXEC (AH=4Bh: load and run, load
 * only, load overlay) and the COMMAND.COM-like entry point dos_start_program. */
#define _POSIX_C_SOURCE 200809L /* fds, directories, localtime_r on any libc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos_int.h"
#include "lift.h"
#include "native/native_exec.h"

enum {
    MZ_LAST_PAGE = 0x02, MZ_PAGES = 0x04, MZ_NRELOC = 0x06, MZ_HDR_PARAS = 0x08,
    MZ_MIN_ALLOC = 0x0A, MZ_MAX_ALLOC = 0x0C, MZ_SS = 0x0E, MZ_SP = 0x10, MZ_IP = 0x14,
    MZ_CS = 0x16, MZ_RELOC_OFF = 0x18, MZ_HEADER_MIN = 0x1C, MZ_PAGE = 512,
};
enum { PROGRAM_MAX = 0xA0000, ENV_MAX = 0x8000, COM_STACK_PARAS = 0x10, COM_ENTRY = 0x100 };
enum { EXEC_LOAD_RUN = 0, EXEC_LOAD_ONLY = 1, EXEC_OVERLAY = 3 };
/* Parameter block of AH=4Bh. */
enum { PB_ENV = 0, PB_TAIL = 2, PB_FCB1 = 6, PB_FCB2 = 0x0A, PB_SSSP = 0x0E, PB_CSIP = 0x12,
       PB_OVL_SEG = 0, PB_OVL_RELOC = 2 };
/* Registers MS-DOS 5 hands a new program. */
enum { ENTRY_CX = 0x00FF, ENTRY_BP = 0x091C, ENTRY_FLAGS = 0x0202 };

typedef struct Program {
    uint8_t *data;
    size_t size;
    bool is_exe;
    size_t image_off, image_size;
    char dos_path[DOS_PATH_MAX];
} Program;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int read_program(Machine *m, const char *name, Program *p)
{
    memset(p, 0, sizeof *p);
    DosPath path;
    int err = dos_resolve(m, name, &path);
    if (err)
        return err;
    if (!path.exists || path.is_dir || path.dev)
        return DE_FILE_NOT_FOUND;
    FILE *f = fopen(path.host, "rb");
    if (!f)
        return DE_ACCESS_DENIED;
    p->data = malloc(PROGRAM_MAX);
    if (!p->data)
        fatal("out of memory");
    p->size = fread(p->data, 1, PROGRAM_MAX, f);
    fclose(f);
    snprintf(p->dos_path, sizeof p->dos_path, "%s", path.dos);
    p->is_exe = p->size >= MZ_HEADER_MIN && (!memcmp(p->data, "MZ", 2) || !memcmp(p->data, "ZM", 2));
    p->image_off = 0;
    p->image_size = p->size;
    if (p->is_exe) {
        size_t total = (size_t)rd16(p->data + MZ_PAGES) * MZ_PAGE;
        uint16_t last = rd16(p->data + MZ_LAST_PAGE);
        if (last)
            total -= MZ_PAGE - last;
        p->image_off = (size_t)rd16(p->data + MZ_HDR_PARAS) * PARA;
        if (p->image_off > p->size || total < p->image_off) {
            free(p->data);
            p->data = NULL;
            return DE_BAD_FORMAT;
        }
        p->image_size = (total < p->size ? total : p->size) - p->image_off;
    }
    return DE_OK;
}

/* Copies the image to load_seg and applies its relocations with reloc_seg. */
static void load_image(Machine *m, const Program *p, uint16_t load_seg, uint16_t reloc_seg)
{
    mem_write_block(m, real_addr(load_seg, 0), p->data + p->image_off, p->image_size);
    if (!p->is_exe)
        return;
    uint16_t count = rd16(p->data + MZ_NRELOC), table = rd16(p->data + MZ_RELOC_OFF);
    for (uint16_t i = 0; i < count && table + i * 4u + 4 <= p->size; i++) {
        const uint8_t *r = p->data + table + i * 4u;
        uint32_t addr = real_addr((uint16_t)(load_seg + rd16(r + 2)), rd16(r));
        mem_wr16(m, addr, (uint16_t)(mem_rd16(m, addr) + reloc_seg));
    }
}

/* Builds a child environment: the source strings, a count word of 1 and the program path. */
static int build_env(Machine *m, uint16_t src, const char *prog, uint16_t *env_seg)
{
    uint32_t base = real_addr(src, 0);
    size_t len = 0;
    while (len + 1 < ENV_MAX && (mem_rd8(m, base + (uint32_t)len) || mem_rd8(m, base + (uint32_t)len + 1)))
        len++;
    len = len == 0 && !mem_rd8(m, base) ? 1 : len + 2; /* include the closing double NUL */
    size_t total = len + 2 + strlen(prog) + 1;
    uint16_t largest;
    int err = dos_mem_alloc(m, (uint16_t)((total + PARA - 1) / PARA), dos_state(m)->cur_psp, env_seg, &largest);
    if (err)
        return err;
    uint32_t dst = real_addr(*env_seg, 0);
    for (size_t i = 0; i < len; i++)
        mem_wr8(m, dst + (uint32_t)i, mem_rd8(m, base + (uint32_t)i));
    if (len == 1)
        mem_wr8(m, dst + 1, 0), len = 2;
    mem_wr16(m, dst + (uint32_t)len, 1);
    mem_write_block(m, dst + (uint32_t)len + 2, prog, strlen(prog) + 1);
    return DE_OK;
}

/* Allocates the program block: as much as the header asks for, at least its minimum. */
static int alloc_program(Machine *m, const Program *p, uint16_t *seg, uint16_t *paras)
{
    uint32_t image_paras = (uint32_t)((p->image_size + PARA - 1) / PARA) + PSP_PARAS;
    uint32_t min = image_paras + (p->is_exe ? rd16(p->data + MZ_MIN_ALLOC) : COM_STACK_PARAS);
    uint32_t max = p->is_exe ? image_paras + rd16(p->data + MZ_MAX_ALLOC) : 0xFFFF;
    if (max > 0xFFFF)
        max = 0xFFFF;
    if (min > 0xFFFF)
        return DE_NO_MEMORY;
    uint16_t owner = dos_state(m)->cur_psp, largest;
    if (dos_mem_alloc(m, (uint16_t)max, owner, seg, &largest) == DE_OK) {
        *paras = (uint16_t)max;
        return DE_OK;
    }
    if (largest < min)
        return DE_NO_MEMORY;
    *paras = largest;
    return dos_mem_alloc(m, largest, owner, seg, &largest);
}

static void mcb_name_of(const char *dos_path, char name[9])
{
    const char *base = strrchr(dos_path, '\\');
    base = base ? base + 1 : dos_path;
    size_t n = strcspn(base, ".");
    snprintf(name, 9, "%.*s", (int)(n > 8 ? 8 : n), base);
}

/* Where the child starts, and where its stack is. */
typedef struct Entry {
    uint16_t cs, ip, ss, sp;
} Entry;

static Entry program_entry(Machine *m, const Program *p, uint16_t psp, uint16_t paras)
{
    uint16_t load = (uint16_t)(psp + PSP_PARAS);
    if (p->is_exe)
        return (Entry){(uint16_t)(load + rd16(p->data + MZ_CS)), rd16(p->data + MZ_IP),
                       (uint16_t)(load + rd16(p->data + MZ_SS)), rd16(p->data + MZ_SP)};
    uint16_t sp = paras >= 0x1000 ? 0xFFFE : (uint16_t)(paras * PARA - 2);
    mem_wr16(m, real_addr(psp, sp), 0);
    return (Entry){psp, COM_ENTRY, psp, sp};
}

/* Sets the registers a new program sees; with an IRET frame when returning from INT 21h. */
static void enter_program(Machine *m, Entry e, uint16_t psp, uint16_t ax, bool via_iret)
{
    Cpu *c = &m->cpu;
    AX = ax, BX = 0, CX = ENTRY_CX, DX = psp, SI = e.ip, DI = e.sp, BP = ENTRY_BP;
    cpu_set_seg(c, S_DS, psp);
    cpu_set_seg(c, S_ES, psp);
    cpu_set_seg(c, S_SS, e.ss);
    if (!via_iret) {
        SP = e.sp;
        cpu_set_seg(c, S_CS, e.cs);
        c->eip = e.ip;
        c->halted = false;
        return;
    }
    SP = (uint16_t)(e.sp - 6);
    uint32_t frame = real_addr(e.ss, SP);
    mem_wr16(m, frame, e.ip);
    mem_wr16(m, frame + 2, e.cs);
    mem_wr16(m, frame + 4, ENTRY_FLAGS);
}

typedef struct ExecArgs {
    uint16_t env_src;
    uint8_t tail[128]; /* length byte, text, CR */
    uint8_t fcb1[16], fcb2[16];
} ExecArgs;

static uint8_t fcb_drive_status(const uint8_t *fcb)
{
    return fcb[0] == 0 || fcb[0] == DOS_DRIVE_C + 1 ? 0 : 0xFF;
}

static bool exec_log_on(void)
{
    const char *v = getenv("INN_EXEC_LOG");
    return v && strcmp(v, "1") == 0;
}

/* INN_EXEC_LOG=1: one line per load, so a native run can be compared with a hybrid run. */
static void log_exec(Machine *m, const char *dos_path, const uint8_t *tail)
{
    if (!exec_log_on())
        return;
    unsigned n = tail[0] > 126 ? 126 : tail[0];
    char text[127];
    unsigned o = 0;
    for (unsigned i = 0; i < n; i++) {
        char c = (char)tail[1 + i];
        if (c == '\r' || c == '\n' || c == 0)
            break;
        text[o++] = c;
    }
    text[o] = 0;
    const DosState *d = dos_state(m);
    log_msg("exec: %s tail=%s cwd=%c:%s", dos_path, text, 'A' + d->cur_drive, d->cwd);
}

/* tail is a length byte, text and CR. FCBs are parsed from the text, as COMMAND.COM does. */
static void exec_args_from_tail(ExecArgs *a, const uint8_t *tail, uint16_t env_src)
{
    memset(a, 0, sizeof *a);
    a->env_src = env_src;
    unsigned n = tail ? tail[0] : 0;
    if (n > sizeof a->tail - 2)
        n = (unsigned)(sizeof a->tail - 2);
    a->tail[0] = (uint8_t)n;
    if (n)
        memcpy(a->tail + 1, tail + 1, n);
    a->tail[n + 1] = '\r';
    char text[sizeof a->tail];
    memcpy(text, a->tail + 1, n);
    text[n] = 0;
    uint8_t result;
    memset(a->fcb1 + 1, ' ', 11);
    memset(a->fcb2 + 1, ' ', 11);
    int used = dos_parse_name_fcb(text, 1, a->fcb1, &result);
    dos_parse_name_fcb(text + used, 1, a->fcb2, &result);
}

/* Loads p as a new process. On success *psp and *entry describe it; memory is owned by it. */
static int create_process(Machine *m, const Program *p, const ExecArgs *a, uint16_t *psp, Entry *entry)
{
    DosState *d = dos_state(m);
    uint16_t env, seg, paras;
    uint16_t env_src = a->env_src ? a->env_src : mem_rd16(m, real_addr(d->cur_psp, PSP_ENV));
    int err = build_env(m, env_src, p->dos_path, &env);
    if (err)
        return err;
    if ((err = alloc_program(m, p, &seg, &paras))) {
        dos_mem_free(m, env);
        return err;
    }
    char name[9];
    mcb_name_of(p->dos_path, name);
    dos_mem_set_owner(m, env, seg, "");
    dos_mem_set_owner(m, seg, seg, name);
    dos_build_psp(m, seg, d->cur_psp, (uint16_t)(seg + paras), env);
    dos_inherit_handles(m, d->cur_psp, seg);
    mem_write_block(m, real_addr(seg, PSP_TAIL), a->tail, sizeof a->tail);
    mem_write_block(m, real_addr(seg, PSP_FCB1), a->fcb1, sizeof a->fcb1);
    mem_write_block(m, real_addr(seg, PSP_FCB2), a->fcb2, sizeof a->fcb2);
    load_image(m, p, (uint16_t)(seg + PSP_PARAS), (uint16_t)(seg + PSP_PARAS));
    /* The whole block, not the file image: packed programs unpack in place and grow. */
    const char *base = strrchr(p->dos_path, '\\');
    lift_module_loaded(m, base ? base + 1 : p->dos_path, (uint16_t)(seg + PSP_PARAS),
                       (uint32_t)(paras - PSP_PARAS) * PARA);
    *psp = seg;
    *entry = program_entry(m, p, seg, paras);
    if (m->trace)
        log_msg("dos: loaded %s psp=%04X paras=%04X entry=%04X:%04X", p->dos_path, seg, paras, entry->cs, entry->ip);
    return DE_OK;
}

static bool push_frame(Machine *m, uint16_t child, bool from_shell)
{
    DosState *d = dos_state(m);
    if (d->depth >= DOS_EXEC_DEPTH)
        return false;
    ExecFrame *f = &d->frames[d->depth++];
    f->child_psp = child;
    f->from_shell = from_shell;
    memcpy(f->r, m->cpu.r, sizeof f->r);
    memcpy(f->seg, m->cpu.seg, sizeof f->seg);
    return true;
}

/* Makes psp current and records how to get back to the parent. */
static void switch_to_child(Machine *m, uint16_t psp, uint16_t ret_cs, uint16_t ret_ip)
{
    DosState *d = dos_state(m);
    uint32_t parent = real_addr(d->cur_psp, 0);
    mem_wr16(m, parent + PSP_SSSP, SP);
    mem_wr16(m, parent + PSP_SSSP + 2, SS);
    mem_wr16(m, real_addr(psp, PSP_INT22), ret_ip);
    mem_wr16(m, real_addr(psp, PSP_INT22 + 2), ret_cs);
    mem_wr16(m, 0x22 * 4, ret_ip);
    mem_wr16(m, 0x22 * 4 + 2, ret_cs);
    d->cur_psp = psp;
    d->dta_seg = psp;
    d->dta_off = PSP_TAIL;
}

static void read_exec_args(Machine *m, uint32_t pb, ExecArgs *a)
{
    a->env_src = mem_rd16(m, pb + PB_ENV);
    uint32_t tail = real_addr(mem_rd16(m, pb + PB_TAIL + 2), mem_rd16(m, pb + PB_TAIL));
    mem_read_block(m, tail, a->tail, sizeof a->tail);
    if (a->tail[0] > sizeof a->tail - 2)
        a->tail[0] = sizeof a->tail - 2;
    a->tail[a->tail[0] + 1] = '\r';
    mem_read_block(m, real_addr(mem_rd16(m, pb + PB_FCB1 + 2), mem_rd16(m, pb + PB_FCB1)), a->fcb1, sizeof a->fcb1);
    mem_read_block(m, real_addr(mem_rd16(m, pb + PB_FCB2 + 2), mem_rd16(m, pb + PB_FCB2)), a->fcb2, sizeof a->fcb2);
}

static CallbackResult exec_overlay(Machine *m, const Program *p)
{
    uint32_t pb = real_addr(ES, BX);
    load_image(m, p, mem_rd16(m, pb + PB_OVL_SEG), mem_rd16(m, pb + PB_OVL_RELOC));
    return dos_ok(m);
}

static CallbackResult exec_process(Machine *m, const Program *p, uint8_t mode)
{
    ExecArgs a;
    uint32_t pb = real_addr(ES, BX);
    read_exec_args(m, pb, &a);
    uint16_t psp;
    Entry e;
    int err = create_process(m, p, &a, &psp, &e);
    if (err)
        return dos_fail(m, (uint16_t)err);
    uint32_t frame = real_addr(SS, SP);
    uint16_t ret_ip = mem_rd16(m, frame), ret_cs = mem_rd16(m, frame + 2);
    uint16_t ax = (uint16_t)(fcb_drive_status(a.fcb1) | fcb_drive_status(a.fcb2) << 8);
    if (!push_frame(m, psp, false)) {
        dos_mem_free_owner(m, psp);
        return dos_fail(m, DE_NO_MEMORY);
    }
    switch_to_child(m, psp, ret_cs, ret_ip);
    log_exec(m, p->dos_path, a.tail);
    if (mode == EXEC_LOAD_ONLY) {
        e.sp = (uint16_t)(e.sp - 2);
        mem_wr16(m, real_addr(e.ss, e.sp), ax);
        mem_wr16(m, pb + PB_SSSP, e.sp);
        mem_wr16(m, pb + PB_SSSP + 2, e.ss);
        mem_wr16(m, pb + PB_CSIP, e.ip);
        mem_wr16(m, pb + PB_CSIP + 2, e.cs);
        return dos_ok(m);
    }
    enter_program(m, e, psp, ax, true);
    return dos_ok(m);
}

CallbackResult dos_exec(Machine *m)
{
    uint8_t mode = AL;
    if (mode != EXEC_LOAD_RUN && mode != EXEC_LOAD_ONLY && mode != EXEC_OVERLAY)
        return dos_fail(m, DE_FUNCTION);
    if (mode == EXEC_LOAD_RUN)
        native_dump_point(m, "exec");
    char name[DOS_PATH_MAX];
    dos_read_name(m, DS, DX, name);
    Program p;
    int err = read_program(m, name, &p);
    CallbackResult r = err ? dos_fail(m, (uint16_t)err)
                     : mode == EXEC_OVERLAY ? exec_overlay(m, &p) : exec_process(m, &p, mode);
    free(p.data);
    return r;
}

/* ---- top-level start, as COMMAND.COM would ---- */

static void args_to_exec(const char *args, ExecArgs *a)
{
    memset(a, 0, sizeof *a);
    char tail[sizeof a->tail];
    snprintf(tail, sizeof tail - 2, "%s%s", args[0] && args[0] != ' ' ? " " : "", args);
    size_t len = strlen(tail);
    a->tail[0] = (uint8_t)len;
    memcpy(a->tail + 1, tail, len);
    a->tail[len + 1] = '\r';
    uint8_t result;
    memset(a->fcb1 + 1, ' ', 11);
    memset(a->fcb2 + 1, ' ', 11);
    int used = dos_parse_name_fcb(tail, 1, a->fcb1, &result);
    dos_parse_name_fcb(tail + used, 1, a->fcb2, &result);
}

static int read_with_ext(Machine *m, const char *dos_path, Program *p)
{
    static const char *const exts[] = {"", ".COM", ".EXE"};
    const char *base = strrchr(dos_path, '\\');
    bool has_ext = strchr(base ? base : dos_path, '.') != NULL;
    int err = DE_FILE_NOT_FOUND;
    for (size_t i = has_ext ? 0 : 1; i < (has_ext ? 1 : 3) && err; i++) {
        char name[DOS_PATH_MAX];
        snprintf(name, sizeof name, "%s%s", dos_path, exts[i]);
        err = read_program(m, name, p);
    }
    return err;
}

bool dos_start_program(Machine *m, const char *dos_path, const char *args)
{
    DosState *d = dos_state(m);
    d->cur_psp = d->shell_psp;
    Program p;
    int err = read_with_ext(m, dos_path, &p);
    ExecArgs a;
    args_to_exec(args ? args : "", &a);
    uint16_t psp;
    Entry e;
    if (!err)
        err = create_process(m, &p, &a, &psp, &e);
    free(p.data);
    if (err || !push_frame(m, err ? 0 : psp, true)) {
        log_msg("dos: cannot start %s: error %d", dos_path, err);
        return false;
    }
    switch_to_child(m, psp, mem_rd16(m, 0x22 * 4 + 2), mem_rd16(m, 0x22 * 4));
    uint16_t ax = (uint16_t)(fcb_drive_status(a.fcb1) | fcb_drive_status(a.fcb2) << 8);
    enter_program(m, e, psp, ax, false);
    log_exec(m, p.dos_path, a.tail);
    native_dump_point(m, "start");
    return true;
}

int dos_exec_load(Machine *m, const char *dos_path, const uint8_t *tail, uint16_t env_seg,
                  DosExecImage *out)
{
    Program p;
    int err = read_program(m, dos_path, &p);
    if (err)
        return err;
    ExecArgs a;
    exec_args_from_tail(&a, tail, env_seg);
    uint16_t psp;
    Entry e;
    err = create_process(m, &p, &a, &psp, &e);
    if (!err) {
        uint16_t ax = (uint16_t)(fcb_drive_status(a.fcb1) | fcb_drive_status(a.fcb2) << 8);
        if (!push_frame(m, psp, false)) {
            dos_mem_free_owner(m, psp);
            err = DE_NO_MEMORY;
        } else {
            /* Vector 22h is whatever the caller installed; AH=4Bh uses the INT 21h return instead. */
            uint16_t ret_ip = mem_rd16(m, 0x22 * 4), ret_cs = mem_rd16(m, 0x22 * 4 + 2);
            switch_to_child(m, psp, ret_cs, ret_ip);
            if (out) {
                out->psp = psp;
                out->cs = e.cs;
                out->ip = e.ip;
                out->ss = e.ss;
                out->sp = e.sp;
                out->ax = ax;
            }
            log_exec(m, p.dos_path, a.tail);
        }
    }
    free(p.data);
    return err;
}

void dos_exec_enter(Machine *m, const DosExecImage *img)
{
    Entry e = {.cs = img->cs, .ip = img->ip, .ss = img->ss, .sp = img->sp};
    enter_program(m, e, img->psp, img->ax, false);
    native_dump_point(m, "start");
}
