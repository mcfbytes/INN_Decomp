/* Native EXEC (docs/native.md sections 9 and 11): a child that starts at a lifted entry runs as C, any
 * other on the emulator; the DOS terminate hook ends it either way. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "dos_int.h"
#include "lift_internal.h"
#include "native_exec.h"

enum { EXEC_FRAMES = 4, TERMINATE_KEEP_NONE = -1, SP_TOP_BYTES = 4 };

typedef struct ExecCtx {
    jmp_buf jump;
    uint16_t parent_psp;  /* the process that started the child: current again once the child ended */
    bool native;          /* the child is C on the host stack, so its termination must longjmp */
    bool finished;
    int saved_nesting, saved_c_depth;
    bool saved_native;
    GpReg saved_r[8];
    uint16_t saved_seg[6];
    uint32_t saved_eip, saved_flags;
    uint8_t saved_stack_top[SP_TOP_BYTES];
    uint32_t saved_vec22;
} ExecCtx;

/* One machine per process runs native programs; frames nest as the programs exec each other. */
static ExecCtx frames[EXEC_FRAMES];
static int frame_count;
static int return_stub_id = -1;
static Machine *return_stub_machine;

static ExecCtx *innermost(void) { return frame_count > 0 ? &frames[frame_count - 1] : NULL; }

/* The innermost child is gone: leave the host stack of a native child, or end the emulator loop. */
static void child_finished(Machine *m)
{
    ExecCtx *fr = innermost();
    fr->finished = true;
    if (fr->native)
        longjmp(fr->jump, 1);
    m->cpu.stop = true;
}

/* Runs after dos_terminate freed a process. Only the end of the process this frame started counts, not
 * a grandchild's. */
static void on_terminate(Machine *m, uint8_t code)
{
    ExecCtx *fr = innermost();
    if (fr && dos_state(m)->cur_psp == fr->parent_psp)
        child_finished(m);
}

/* Vector 22h while a child runs: a terminate that reaches the stub through the CPU ends the frame too. */
static CallbackResult exec_returned(Machine *m)
{
    if (innermost())
        child_finished(m);
    return CB_DONE;
}

static void ensure_stub(Machine *m)
{
    if (return_stub_machine != m || return_stub_id < 0 || m->callbacks[return_stub_id] != exec_returned) {
        return_stub_id = callback_register(m, "native exec return", exec_returned, -1);
        return_stub_machine = m;
    }
    dos_set_terminate_hook(m, on_terminate);
}

static uint32_t stub_vector(void)
{
    uint32_t stub = callback_stub_addr(return_stub_id);
    return (uint32_t)CALLBACK_SEG << 16 | (uint16_t)(stub - real_addr(CALLBACK_SEG, 0));
}

static void save_state(Machine *m, ExecCtx *fr)
{
    LiftState *s = lift_state(m);
    fr->saved_nesting = s->nesting;
    fr->saved_c_depth = s->c_depth;
    fr->saved_native = m->native;
    memcpy(fr->saved_r, m->cpu.r, sizeof fr->saved_r);
    memcpy(fr->saved_seg, m->cpu.seg, sizeof fr->saved_seg);
    fr->saved_eip = m->cpu.eip;
    fr->saved_flags = m->cpu.flags;
    /* Terminate writes the parent's resume address at SS:SP; the C caller has live data there. */
    mem_read_block(m, real_addr(SS, SP), fr->saved_stack_top, SP_TOP_BYTES);
    fr->saved_vec22 = mem_rd32(m, 0x22 * 4);
}

static void restore_state(Machine *m, ExecCtx *fr)
{
    LiftState *s = lift_state(m);
    s->nesting = fr->saved_nesting;
    s->c_depth = fr->saved_c_depth;
    m->native = fr->saved_native;
    memcpy(m->cpu.r, fr->saved_r, sizeof m->cpu.r);
    for (int i = 0; i < 6; i++)
        cpu_set_seg(&m->cpu, i, fr->saved_seg[i]);
    m->cpu.eip = fr->saved_eip;
    m->cpu.flags = fr->saved_flags;
    m->cpu.stop = false;
    mem_write_block(m, real_addr(SS, SP), fr->saved_stack_top, SP_TOP_BYTES);
    mem_wr32(m, 0x22 * 4, fr->saved_vec22);
}

/* The lifted entry at CS:IP of a program image, if a hook is armed there with matching bytes. */
static bool native_start_at(Machine *m, uint16_t cs, uint16_t ip)
{
    uint32_t linear = real_addr(cs, ip);
    if (!lift_hooked(m, linear))
        return false;
    LiftState *s = lift_state(m);
    for (int i = 0; i < s->active_count; i++)
        if (s->active[i].linear == linear)
            return lift_entry_bytes_match(m, &s->active[i]);
    return false;
}

/* Runs the child whose registers are set up until it terminates. Returns through the jump buffer when
 * a native child terminates, normally when an emulated one does. */
static void run_child(Machine *m, ExecCtx *fr)
{
    if (setjmp(fr->jump) != 0)
        return;
    LiftState *s = lift_state(m);
    uint16_t cs = m->cpu.seg[S_CS], ip = (uint16_t)m->cpu.eip;
    fr->native = native_start_at(m, cs, ip);
    if (fr->native) {
        /* M9 stand-in until package 11: lift_dispatch_far_at; guest_call_far pushes a return sentinel. */
        guest_call_far(m, cs, ip);
        log_msg("native_exec: the program at %04X:%04X returned instead of terminating; exit code 0", cs, ip);
        dos_terminate(m, 0, 0, TERMINATE_KEEP_NONE); /* ends the frame through the hook */
        return;
    }
    native_handoff_to_emulator(m);
    m->native = false;
    s->c_depth = 0;
    while (!fr->finished && !m->exit_requested && !s->nested_abort) {
        machine_step(m);
        if (s->nested_yield && !s->in_shadow)
            s->nested_yield(m);
    }
    native_handoff_from_emulator(m);
}

void native_handoff_to_emulator(Machine *m) { (void)m; }
void native_handoff_from_emulator(Machine *m) { (void)m; }

int native_exec(Machine *m, const char *dos_path, const uint8_t *tail, uint16_t env_seg, uint16_t *dos_error)
{
    native_dump_point(m, "exec");
    if (frame_count >= EXEC_FRAMES) {
        *dos_error = DE_NO_MEMORY;
        return -1;
    }
    ensure_stub(m);
    ExecCtx *fr = &frames[frame_count];
    memset(&fr->jump, 0, sizeof fr->jump);
    fr->finished = false;
    fr->native = false;
    fr->parent_psp = dos_state(m)->cur_psp;
    save_state(m, fr);
    mem_wr32(m, 0x22 * 4, stub_vector()); /* the child's PSP records this as its return address */
    DosExecImage img;
    int err = dos_exec_load(m, dos_path, tail, env_seg, &img);
    if (err) {
        restore_state(m, fr);
        *dos_error = (uint16_t)err;
        return -1;
    }
    dos_exec_enter(m, &img);
    frame_count++;
    run_child(m, fr);
    frame_count--;
    restore_state(m, fr);
    if (!fr->finished)
        return 0; /* quit requested while the child ran; m->exit_requested is set */
    return dos_last_exit_status(m) & 0xFF;
}

void native_run_top(Machine *m)
{
    if (frame_count >= EXEC_FRAMES)
        fatal("native_run_top: exec frames exhausted");
    ensure_stub(m);
    ExecCtx *fr = &frames[frame_count];
    memset(&fr->jump, 0, sizeof fr->jump);
    fr->finished = false;
    fr->native = false;
    fr->parent_psp = mem_rd16(m, real_addr(dos_state(m)->cur_psp, PSP_PARENT));
    LiftState *s = lift_state(m);
    fr->saved_nesting = s->nesting;
    fr->saved_c_depth = s->c_depth;
    fr->saved_native = m->native;
    frame_count++;
    run_child(m, fr);
    frame_count--;
    s->nesting = fr->saved_nesting;
    s->c_depth = fr->saved_c_depth;
    m->native = fr->saved_native;
    m->cpu.stop = false;
}
