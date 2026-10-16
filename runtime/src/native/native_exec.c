/* Native EXEC (docs/native.md sections 9 and 11): a child that starts at a lifted entry runs as C, any
 * other on the emulator; the DOS terminate hook ends it either way. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "devices.h"
#include "dos_int.h"
#include "host_link.h"
#include "host_timer.h"
#include "host_video.h"
#include "lift_internal.h"
#include "native.h"
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
static bool native_start_at(Machine *m, uint16_t cs, uint16_t ip, bool native_only)
{
    uint32_t linear = real_addr(cs, ip);
    if (!lift_hooked(m, linear))
        return false;
    LiftState *s = lift_state(m);
    for (int i = 0; i < s->active_count; i++)
        if (s->active[i].linear == linear)
            return (!native_only || s->active[i].entry->native_only) && lift_entry_bytes_match(m, &s->active[i]);
    return false;
}

/* The packed program's unpacker runs on the CPU, with interrupts held so that none goes to a device,
 * until it jumps to the program's native start. The CPU runs only the unpacker, never guest code of
 * the client, so native mode is off for it (strict mode would refuse the instructions). False when it
 * stops anywhere else first (another lifted entry, a halt, the budget). */
enum { UNPACK_INSNS = 100000000 };

bool native_unpack_to_start(Machine *m)
{
    bool was_native = m->native, hold = m->irq_hold;
    bool found = false;
    m->native = false;
    m->irq_hold = true;
    for (uint64_t left = UNPACK_INSNS; !m->exit_requested;) {
        if (native_start_at(m, m->cpu.seg[S_CS], (uint16_t)m->cpu.eip, true)) {
            found = true;
            break;
        }
        m->cpu.hook_pending = false;
        uint64_t done = cpu_run(m, left);
        if (m->cpu.hook_pending) {
            m->cpu.hook_pending = false;
            found = native_start_at(m, m->cpu.seg[S_CS], (uint16_t)m->cpu.eip, true);
            break;
        }
        if (done == 0 || done >= left || m->cpu.halted)
            break;
        left -= done;
    }
    m->irq_hold = hold;
    m->native = was_native;
    return found;
}

/* Runs the child whose registers are set up until it terminates. Returns through the jump buffer when
 * a native child terminates, normally when an emulated one does. */
static void run_child(Machine *m, ExecCtx *fr)
{
    if (setjmp(fr->jump) != 0)
        return;
    LiftState *s = lift_state(m);
    uint16_t cs = m->cpu.seg[S_CS], ip = (uint16_t)m->cpu.eip;
    fr->native = native_start_at(m, cs, ip, false);
    if (!fr->native) {
        /* A packed INN program (an LZEXE unpacker at the EXE entry) starts natively once unpacked. */
        uint16_t base;
        const char *module = lift_module_owning(m, cs, &base);
        if (module && lift_module_has_native(m, module) && native_unpack_to_start(m)) {
            cs = m->cpu.seg[S_CS];
            ip = (uint16_t)m->cpu.eip;
            fr->native = true;
        }
    }
    if (fr->native) {
        lift_dispatch_far_at(m, cs, ip);
        log_msg("native_exec: the program at %04X:%04X returned instead of terminating; exit code 0", cs, ip);
        dos_terminate(m, 0, 0, TERMINATE_KEEP_NONE); /* ends the frame through the hook */
        return;
    }
    bool mixed = m->native; /* INN runs natively: the devices change hands around the game */
    if (mixed)
        native_handoff_to_emulator(m);
    m->native = false;
    s->c_depth = 0;
    while (!fr->finished && !m->exit_requested && !s->nested_abort) {
        machine_step(m);
        if (s->nested_yield && !s->in_shadow)
            s->nested_yield(m);
    }
    if (mixed)
        native_handoff_from_emulator(m);
}

/* ---- mixed mode (docs/native.md section 11): the devices change hands around a game ---- */

enum { IVT_VECTORS = 256, IVT_BYTES = IVT_VECTORS * 4, PORT61_GATES = 0x03, PORT61 = 0x61, FIRST_FREE_PARA = 0xA000 };

static uint8_t handoff_ivt[IVT_BYTES]; /* the vectors at hand-off; one game runs at a time */
static uint16_t handoff_divisor;

/* The machine's tickers stand in for the safe points the game does not have. */
static void link_ticker(Machine *m) { host_link_pump(m); }

/* True when linear lies in a free block of the DOS arena, memory a program that ended gave back. */
static bool in_free_memory(Machine *m, uint32_t linear)
{
    uint32_t para = linear >> 4;
    uint16_t mcb = mem_rd16(m, real_addr(DOS_DATA_SEG, DD_FIRST_MCB));
    for (int guard = 0; guard < 4096; guard++) {
        uint8_t type = mem_rd8(m, real_addr(mcb, MCB_TYPE));
        if (type != 'M' && type != 'Z')
            return false;
        uint32_t start = (uint32_t)mcb + 1u, end = start + mem_rd16(m, real_addr(mcb, MCB_SIZE));
        if (para >= start && para < end)
            return mem_rd16(m, real_addr(mcb, MCB_OWNER)) == 0;
        if (type == 'Z')
            return para >= end && para < FIRST_FREE_PARA;
        mcb = (uint16_t)end;
    }
    return false;
}

/* A vector the game left pointing into memory that is free again is put back to what it was; native
 * code would otherwise call into whatever DOS loads there next. */
static void restore_stale_vectors(Machine *m)
{
    for (int v = 0; v < IVT_VECTORS; v++) {
        uint32_t now = mem_rd32(m, (uint32_t)v * 4), before;
        memcpy(&before, handoff_ivt + v * 4, sizeof before);
        if (now == before)
            continue;
        uint16_t off = (uint16_t)now, seg = (uint16_t)(now >> 16);
        if (!in_free_memory(m, real_addr(seg, off)))
            continue;
        log_msg("native: vector %02X left at %04X:%04X by the game", v, seg, off);
        mem_wr32(m, (uint32_t)v * 4, before);
    }
    uint16_t seg, off;
    if (mouse_handler(m, &seg, &off) && in_free_memory(m, real_addr(seg, off))) {
        log_msg("native: mouse handler left at %04X:%04X by the game", seg, off);
        mouse_drop_handler(m);
    }
}

/* In this order: the mode switch (video follows it), the timer, port 61h's speaker gates, the PIC and
 * the keyboard bytes, a mouse event waiting for its IRQ, the link ticker. Nothing of the native model
 * keeps a copy of what it gave away. */
void native_handoff_to_emulator(Machine *m)
{
    mem_read_block(m, 0, handoff_ivt, IVT_BYTES);
    handoff_divisor = host_timer_divisor(m);
    uint8_t port61 = native_port61(m);
    uint64_t next_tick = host_timer_next_due_us(m); /* the PIT goes on counting where the native timer is */
    native_set_active(m, false); /* m->native and m->native_video off; host_video_to_vga */
    pit_set_timer_divisor(m, handoff_divisor, next_tick > m->virtual_us ? next_tick - m->virtual_us : 0);
    port_out(m, PORT61, port61 & PORT61_GATES, 1);
    native_irq_to_pic(m);
    if (mouse_queued(m))
        pic_raise_irq(m, PIC_IRQ_MOUSE);
    machine_add_ticker(m, link_ticker);
}

/* The reverse: the ticker goes, the video comes back in the mode the VGA device has (text mode if a game
 * left one the host video does not carry), the native timer takes the PIT's divisor, the speaker gates
 * come back, masks come from the PIC, and vectors and the mouse handler that point into freed memory are
 * put right. */
void native_handoff_from_emulator(Machine *m)
{
    machine_remove_ticker(m, link_ticker);
    m->cpu.halted = m->cpu.yielded = false;
    uint8_t mode = vga_mode(m);
    if (mode != HOST_VIDEO_TEXT80 && mode != HOST_VIDEO_320X200X8) {
        log_msg("native: the game left video mode %02X; back to text mode", mode);
        vga_set_mode(m, HOST_VIDEO_TEXT80, true);
    }
    uint8_t gates = (uint8_t)port_in(m, PORT61, 1) & PORT61_GATES;
    native_set_active(m, true); /* host_video_init takes the VGA device's picture */
    host_timer_set_divisor(m, pit_timer_divisor(m));
    native_set_port61(m, (uint8_t)((native_port61(m) & ~PORT61_GATES) | gates));
    native_irq_from_pic(m);
    restore_stale_vectors(m);
}

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
