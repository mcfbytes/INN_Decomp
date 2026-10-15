/* Processes: PSP construction, the root shell, terminate / TSR (with return to the parent's
 * INT 22h address), and the PSP get/set/create functions. */
#define _POSIX_C_SOURCE 200809L /* fds, directories, localtime_r on any libc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos_int.h"
#include "lift.h"

static void dos_audit_arm(Machine *m);

enum { INT22 = 0x22, INT23 = 0x23, INT24 = 0x24 };
enum { TERM_NORMAL = 0, TERM_TSR = 3, TSR_MIN_PARAS = 6, KEEP_NONE = -1 };
enum { PSP_BYTES = 0x100, FCB_BLANK_LEN = 11, DOS_VERSION_WORD = 0x0005 };
/* CP/M-style far call at PSP:05 into the classic wrap-around entry. */
enum { CALL5_OFF = 0xFEF0, CALL5_SEG = 0xF01D, OP_CALL_FAR = 0x9A };

static const char shell_env[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0PROMPT=$P$G\0BLASTER=A220 I7 D1 T3\0";
static const char shell_path[] = "C:\\COMMAND.COM";
enum { SHELL_PSP_PARAS = 0x20, OWNER_DOS = 8 };

static uint32_t vector_addr(int n) { return (uint32_t)n * 4; }

void dos_build_psp(Machine *m, uint16_t psp, uint16_t parent, uint16_t top, uint16_t env)
{
    uint32_t b = real_addr(psp, 0);
    for (uint32_t i = 0; i < PSP_BYTES; i++)
        mem_wr8(m, b + i, 0);
    mem_wr16(m, b + PSP_INT20, 0x20CD);
    mem_wr16(m, b + PSP_TOP, top);
    mem_wr8(m, b + PSP_CALL5, OP_CALL_FAR);
    mem_wr16(m, b + PSP_CALL5 + 1, CALL5_OFF);
    mem_wr16(m, b + PSP_CALL5 + 3, CALL5_SEG);
    mem_wr32(m, b + PSP_INT22, mem_rd32(m, vector_addr(INT22)));
    mem_wr32(m, b + PSP_INT23, mem_rd32(m, vector_addr(INT23)));
    mem_wr32(m, b + PSP_INT24, mem_rd32(m, vector_addr(INT24)));
    mem_wr16(m, b + PSP_PARENT, parent);
    for (int h = 0; h < DOS_JFT_DEFAULT; h++)
        mem_wr8(m, b + PSP_JFT + (uint32_t)h, JFT_CLOSED);
    mem_wr16(m, b + PSP_ENV, env);
    mem_wr16(m, b + PSP_JFT_SIZE, DOS_JFT_DEFAULT);
    mem_wr16(m, b + PSP_JFT_PTR, PSP_JFT);
    mem_wr16(m, b + PSP_JFT_PTR + 2, psp);
    mem_wr32(m, b + PSP_PREV, 0xFFFFFFFFu);
    mem_wr16(m, b + PSP_VERSION, DOS_VERSION_WORD);
    mem_wr8(m, b + PSP_DISPATCH, 0xCD); /* int 21h; retf */
    mem_wr8(m, b + PSP_DISPATCH + 1, 0x21);
    mem_wr8(m, b + PSP_DISPATCH + 2, 0xCB);
    for (int i = 0; i < FCB_BLANK_LEN; i++) {
        mem_wr8(m, b + PSP_FCB1 + 1 + (uint32_t)i, ' ');
        mem_wr8(m, b + PSP_FCB2 + 1 + (uint32_t)i, ' ');
    }
    mem_wr8(m, b + PSP_TAIL + 1, '\r');
}

/* The root process standing in for COMMAND.COM: owns the standard handles, parent of itself. */
void dos_create_shell(Machine *m)
{
    DosState *d = dos_state(m);
    uint16_t env, psp, largest;
    size_t env_len = sizeof shell_env + 2 + sizeof shell_path;
    if (dos_mem_alloc(m, (uint16_t)((env_len + PARA - 1) / PARA), OWNER_DOS, &env, &largest) ||
        dos_mem_alloc(m, SHELL_PSP_PARAS, OWNER_DOS, &psp, &largest))
        fatal("dos: cannot allocate the shell");
    uint32_t e = real_addr(env, 0);
    mem_write_block(m, e, shell_env, sizeof shell_env);
    mem_wr16(m, e + sizeof shell_env, 1);
    mem_write_block(m, e + sizeof shell_env + 2, shell_path, sizeof shell_path);
    dos_mem_set_owner(m, env, psp, "");
    dos_mem_set_owner(m, psp, psp, "COMMAND");
    dos_build_psp(m, psp, psp, (uint16_t)(psp + SHELL_PSP_PARAS), env);
    static const uint8_t std_sft[] = {0, 0, 0, 1, 2}; /* stdin, stdout, stderr = CON, AUX, PRN */
    for (uint16_t h = 0; h < sizeof std_sft; h++) {
        dos_jft_set(m, psp, h, std_sft[h]);
        d->sft[std_sft[h]].refs++;
    }
    d->shell_psp = d->cur_psp = psp;
    d->dta_seg = psp;
    d->dta_off = PSP_TAIL;
    dos_audit_arm(m);
}

static ExecFrame *pop_frame(DosState *d, uint16_t psp)
{
    for (int i = d->depth - 1; i >= 0; i--)
        if (d->frames[i].child_psp == psp) {
            d->depth = i;
            return &d->frames[i];
        }
    return NULL;
}

static bool exec_log_on(void)
{
    const char *v = getenv("INN_EXEC_LOG");
    return v && strcmp(v, "1") == 0;
}

/* Resumes the parent: its registers from EXEC time, or its saved SS:SP as a fallback. */
static void resume_parent(Machine *m, const ExecFrame *f, uint16_t parent, uint32_t ret)
{
    Cpu *c = &m->cpu;
    if (f) {
        memcpy(c->r, f->r, sizeof c->r);
        for (int s = 0; s < 6; s++)
            cpu_set_seg(c, s, f->seg[s]);
    } else {
        uint32_t sssp = mem_rd32(m, real_addr(parent, PSP_SSSP));
        SP = (uint16_t)sssp;
        cpu_set_seg(c, S_SS, (uint16_t)(sssp >> 16));
    }
    uint32_t frame = real_addr(SS, SP);
    mem_wr32(m, frame, ret); /* IP, CS of the INT 21h frame the stub returns through */
}

void dos_terminate(Machine *m, uint8_t code, uint8_t type, int keep_paras)
{
    DosState *d = dos_state(m);
    uint16_t psp = d->cur_psp;
    uint32_t b = real_addr(psp, 0);
    uint16_t parent = mem_rd16(m, b + PSP_PARENT);
    uint32_t ret = mem_rd32(m, b + PSP_INT22);
    mem_wr32(m, vector_addr(INT22), ret);
    mem_wr32(m, vector_addr(INT23), mem_rd32(m, b + PSP_INT23));
    mem_wr32(m, vector_addr(INT24), mem_rd32(m, b + PSP_INT24));
    if (keep_paras == KEEP_NONE) {
        lift_module_unloaded(m, psp, mem_rd16(m, real_addr((uint16_t)(psp - 1), MCB_SIZE)));
        dos_close_all(m, psp);
        dos_mem_free_owner(m, psp);
    } else {
        uint16_t largest;
        dos_mem_resize(m, psp, (uint16_t)(keep_paras < TSR_MIN_PARAS ? TSR_MIN_PARAS : keep_paras), &largest);
    }
    d->return_code = (uint16_t)(code | type << 8);
    if (m->trace)
        log_msg("dos: psp %04X terminated, code %02X type %u", psp, code, type);
    if (exec_log_on())
        log_msg("exit: %04X code=%02X type=%02X", psp, code, type);
    ExecFrame *f = pop_frame(d, psp);
    d->cur_psp = parent;
    d->dta_seg = parent;
    d->dta_off = PSP_TAIL;
    /* Hook last on both paths: it may longjmp. The top program has exit_requested set first. */
    void (*hook)(Machine *, uint8_t) = d->terminate_hook;
    if (psp == d->shell_psp || parent == psp || (f && f->from_shell)) {
        m->exit_requested = true;
        m->exit_code = code;
        if (hook)
            hook(m, code);
        return;
    }
    resume_parent(m, f, parent, ret);
    if (hook)
        hook(m, code);
}

static CallbackResult terminate(Machine *m, uint8_t code, uint8_t type, int keep)
{
    dos_terminate(m, code, type, keep);
    return dos_ok(m);
}

CallbackResult dos_exit(Machine *m) { return terminate(m, AL, TERM_NORMAL, KEEP_NONE); }

CallbackResult dos_exit_old(Machine *m) { return terminate(m, 0, TERM_NORMAL, KEEP_NONE); }

CallbackResult dos_int20(Machine *m) { return terminate(m, 0, TERM_NORMAL, KEEP_NONE); }

CallbackResult dos_keep(Machine *m) { return terminate(m, AL, TERM_TSR, DX); }

CallbackResult dos_int27(Machine *m)
{
    return terminate(m, 0, TERM_TSR, (DX + PARA - 1) / PARA);
}

CallbackResult dos_get_return_code(Machine *m)
{
    AX = dos_state(m)->return_code;
    dos_state(m)->return_code = 0;
    return dos_ok(m);
}

uint16_t dos_last_exit_status(Machine *m)
{
    return dos_state(m)->return_code;
}

void dos_set_terminate_hook(Machine *m, void (*hook)(Machine *m, uint8_t code))
{
    dos_state(m)->terminate_hook = hook;
}

CallbackResult dos_set_psp(Machine *m)
{
    dos_state(m)->cur_psp = BX;
    return CB_DONE;
}

CallbackResult dos_get_psp(Machine *m)
{
    BX = dos_state(m)->cur_psp;
    return CB_DONE;
}

/* AH=26h: copy of the current PSP at DX with the current INT 22h-24h vectors. */
CallbackResult dos_new_psp(Machine *m)
{
    uint32_t src = real_addr(dos_state(m)->cur_psp, 0), dst = real_addr(DX, 0);
    for (uint32_t i = 0; i < PSP_BYTES; i++)
        mem_wr8(m, dst + i, mem_rd8(m, src + i));
    mem_wr32(m, dst + PSP_INT22, mem_rd32(m, vector_addr(INT22)));
    mem_wr32(m, dst + PSP_INT23, mem_rd32(m, vector_addr(INT23)));
    mem_wr32(m, dst + PSP_INT24, mem_rd32(m, vector_addr(INT24)));
    mem_wr16(m, dst + PSP_JFT_PTR, PSP_JFT);
    mem_wr16(m, dst + PSP_JFT_PTR + 2, DX);
    return CB_DONE;
}

/* INT 21h counts for INN_DOS_AUDIT=1. Split by AL where one AH covers several functions. */
enum { AUDIT_AL_FUNCS = 8 };

static const uint8_t audit_al_ah[AUDIT_AL_FUNCS] = {0x44, 0x4B, 0x57, 0x58, 0x5A, 0x5B, 0x5C, 0x5D};
static uint32_t audit_count[256];
static uint32_t audit_al[AUDIT_AL_FUNCS][256];
static CallbackFn audit_int21;

static int audit_al_index(uint8_t ah)
{
    for (int i = 0; i < AUDIT_AL_FUNCS; i++)
        if (audit_al_ah[i] == ah)
            return i;
    return -1;
}

static CallbackResult audited_int21(Machine *m)
{
    int sub = audit_al_index(AH);
    if (sub >= 0)
        audit_al[sub][AL]++;
    else
        audit_count[AH]++;
    return audit_int21(m);
}

static void audit_dump(void)
{
    int sub = 0;
    for (int ah = 0; ah < 256; ah++) {
        if (sub < AUDIT_AL_FUNCS && ah == audit_al_ah[sub]) {
            for (int al = 0; al < 256; al++)
                if (audit_al[sub][al])
                    log_msg("dos audit: AH=%02X AL=%02X %u", ah, al, (unsigned)audit_al[sub][al]);
            sub++;
            continue;
        }
        if (audit_count[ah])
            log_msg("dos audit: AH=%02X %u", ah, (unsigned)audit_count[ah]);
    }
    fflush(stderr);
}

static void dos_audit_arm(Machine *m)
{
    const char *v = getenv("INN_DOS_AUDIT");
    if (!v || strcmp(v, "1") != 0)
        return;
    uint16_t off = mem_rd16(m, 0x21 * 4), seg = mem_rd16(m, 0x21 * 4 + 2);
    uint32_t stub = real_addr(seg, off);
    int id = mem_rd8(m, stub + 2);
    if (seg != CALLBACK_SEG || mem_rd8(m, stub) != 0xFE || mem_rd8(m, stub + 1) != 0x38 ||
        mem_rd8(m, stub + 3) != 0xCF || id >= m->callback_count || !m->callbacks[id]) {
        log_msg("dos audit: INT 21h stub not found");
        return;
    }
    if (!audit_int21)
        audit_int21 = m->callbacks[id];
    m->callbacks[id] = audited_int21;
    static bool hooked;
    if (!hooked) {
        atexit(audit_dump);
        hooked = true;
    }
}

/* AH=55h: a child PSP at DX with memory top SI; it becomes the current PSP. */
CallbackResult dos_child_psp(Machine *m)
{
    DosState *d = dos_state(m);
    uint16_t env = mem_rd16(m, real_addr(d->cur_psp, PSP_ENV));
    dos_build_psp(m, DX, d->cur_psp, SI, env);
    dos_inherit_handles(m, d->cur_psp, DX);
    d->cur_psp = DX;
    return CB_DONE;
}
