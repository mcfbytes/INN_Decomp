/* Idle polls (docs/runtime.md): a guest spinning on input polls that find nothing lets time pass to the
 * next interrupt; a key, a mouse move, a timer tick still arrive in time, and loops that do anything
 * else between polls run as before. */
#include <stdio.h>
#include <string.h>

#include "devices.h"
#include "host_input.h"

static int failures;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            failures++;                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fputc('\n', stderr);                                 \
        }                                                        \
    } while (0)

enum { CODE_SEG = 0x0700, STACK_SEG = 0x0600, STACK_TOP = 0x0FFE, RESULT = 0x500 };

static Machine *boot(const uint8_t *code, size_t len, bool idle)
{
    Machine *m = machine_create();
    bios_init(m);
    m->idle.off = !idle;
    memcpy(m->mem + real_addr(CODE_SEG, 0), code, len);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    cpu_set_seg(&m->cpu, S_DS, 0);
    m->cpu.eip = 0;
    SP = STACK_TOP;
    m->cpu.flags = 0x0202;
    return m;
}

static void done(Machine *m)
{
    bios_shutdown(m);
    machine_destroy(m);
}

/* loop: mov ah,1; int 16h; jz loop; mov ah,0; int 16h; mov [500h],ax; jmp $ */
static const uint8_t KEY_WAIT[] = { 0xB4, 0x01, 0xCD, 0x16, 0x74, 0xFA, 0xB4, 0x00, 0xCD, 0x16,
                                    0xA3, 0x00, 0x05, 0xEB, 0xFE };

static void test_key_wait(void)
{
    Machine *m = boot(KEY_WAIT, sizeof KEY_WAIT, true);
    uint64_t before = m->cpu.instructions;
    machine_run_for(m, 200000);
    uint64_t spun = m->cpu.instructions - before;
    CHECK(spun < 100000, "an idle key wait ran %llu instructions in 200 ms (4M without skipping)",
          (unsigned long long)spun);
    CHECK(m->idle.skipped_us > 150000, "skipped %llu us of 200 ms", (unsigned long long)m->idle.skipped_us);
    host_input_key(m, 0x1E, true);
    host_input_key(m, 0x1E, false);
    machine_run_for(m, 1000);
    CHECK(mem_rd16(m, RESULT) == 0x1E61, "the key arrives within 1 ms: %04X", mem_rd16(m, RESULT));
    done(m);

    m = boot(KEY_WAIT, sizeof KEY_WAIT, false);
    before = m->cpu.instructions;
    machine_run_for(m, 200000);
    CHECK(m->cpu.instructions - before > 3000000 && m->idle.skipped_us == 0, "--no-idle-skip spins: %llu",
          (unsigned long long)(m->cpu.instructions - before));
    done(m);
}

/* Wait for the BIOS tick to advance twice while polling the keyboard; [500h] = 1 when it has.
 *   mov bx,[46Ch]; add bx,2; loop: mov ah,1; int 16h; cmp bx,[46Ch]; jne loop; mov word [500h],1; jmp $ */
static const uint8_t TICK_WAIT[] = { 0x8B, 0x1E, 0x6C, 0x04, 0x83, 0xC3, 0x02, 0xB4, 0x01, 0xCD, 0x16,
                                     0x3B, 0x1E, 0x6C, 0x04, 0x75, 0xF6, 0xC7, 0x06, 0x00, 0x05, 0x01, 0x00,
                                     0xEB, 0xFE };

static uint64_t tick_wait_ms(bool idle, uint64_t *skipped)
{
    Machine *m = boot(TICK_WAIT, sizeof TICK_WAIT, idle);
    uint64_t ms = 0;
    for (; ms < 300 && mem_rd16(m, RESULT) != 1; ms++)
        machine_run_for(m, 1000);
    *skipped = m->idle.skipped_us;
    done(m);
    return ms;
}

static void test_timer_wakes(void)
{
    uint64_t skipped_on, skipped_off;
    uint64_t on = tick_wait_ms(true, &skipped_on), off = tick_wait_ms(false, &skipped_off);
    CHECK(on == off && on > 50 && on < 120, "two ticks: %llu ms with skipping, %llu ms without",
          (unsigned long long)on, (unsigned long long)off);
    CHECK(skipped_on > on * 1000 / 2, "the wait was mostly skipped: %llu us", (unsigned long long)skipped_on);
}

/* Reset the mouse, then: loop: mov ax,3; int 33h; cmp cx,[500h]; je loop; mov [502h],cx; jmp $ */
static const uint8_t MOUSE_WAIT[] = { 0x31, 0xC0, 0xCD, 0x33, 0xB8, 0x03, 0x00, 0xCD, 0x33, 0x89, 0x0E,
                                      0x00, 0x05, 0xB8, 0x03, 0x00, 0xCD, 0x33, 0x3B, 0x0E, 0x00, 0x05,
                                      0x74, 0xF5, 0x89, 0x0E, 0x02, 0x05, 0xEB, 0xFE };

static void test_mouse_poll(void)
{
    Machine *m = boot(MOUSE_WAIT, sizeof MOUSE_WAIT, true);
    machine_run_for(m, 100000);
    CHECK(m->idle.skipped_us > 50000, "an unchanged mouse is idle: skipped %llu us", (unsigned long long)m->idle.skipped_us);
    host_input_mouse_motion(m, 100, 50, 320, 200);
    machine_run_for(m, 1500);
    CHECK(mem_rd16(m, RESULT + 2) == 200, "the move is seen within 1.5 ms: CX=%u", mem_rd16(m, RESULT + 2));
    done(m);
}

/* Busy loops are not idle: port I/O between the polls, or more work than IDLE_GAP_SLOTS.
 *   a: loop: out 80h,al; mov ah,1; int 16h; jz loop; jmp $
 *   b: loop: mov cx,6000; l2: loop l2; mov ah,1; int 16h; jz loop; jmp $ */
static const uint8_t IO_LOOP[] = { 0xE6, 0x80, 0xB4, 0x01, 0xCD, 0x16, 0x74, 0xF8, 0xEB, 0xFE };
static const uint8_t WORK_LOOP[] = { 0xB9, 0x70, 0x17, 0xE2, 0xFE, 0xB4, 0x01, 0xCD, 0x16, 0x74, 0xF5, 0xEB, 0xFE };

static void test_busy_loops(void)
{
    const uint8_t *codes[] = { IO_LOOP, WORK_LOOP };
    size_t lens[] = { sizeof IO_LOOP, sizeof WORK_LOOP };
    for (int i = 0; i < 2; i++) {
        Machine *m = boot(codes[i], lens[i], true);
        machine_run_for(m, 100000);
        CHECK(m->idle.skipped_us == 0, "busy loop %d skipped %llu us", i, (unsigned long long)m->idle.skipped_us);
        done(m);
    }
}

int main(void)
{
    test_key_wait();
    test_timer_wakes();
    test_mouse_poll();
    test_busy_loops();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        printf("test_idle: all passed\n");
    return failures ? 1 : 0;
}
