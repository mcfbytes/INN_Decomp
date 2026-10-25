/* Images loaded by the program (docs/lifting.md): found by their head at the first far call to offset 0,
 * their entries armed then. Also INN_DUMP_AT module address points, reached by far calls. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lift.h"

static int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

enum {
    PROGRAM_SEG = 0x1000, STACK_SEG = 0x2000, STACK_TOP = 0x100,
    IMAGE_A = 0x3000, IMAGE_B = 0x4000, IMAGE_C = 0x5000,
    ENTRY_OFF = 0x0010, /* the image's lifted function: mov ax,1111h / retf */
    ORIGINAL_AX = 0x1111, LIFTED_AX = 0xAAAA,
};

/* The image, as a driver starts: jmp short ENTRY, a name, a relocated word at 14; then its function. */
static const uint8_t IMAGE[] = {
    0xEB, ENTRY_OFF - 2, 'T', 'E', 'S', 'T', 'I', 'M', 'G', 0, 0x21, 0x43, 0x65, 0x87, 0, 0,
    [ENTRY_OFF] = 0xB8, ORIGINAL_AX & 0xFF, ORIGINAL_AX >> 8, 0xCB, 'p', 'a', 'd', 'd', 'i', 'n', 'g', '.',
    'm', 'o', 'r', 'e', 0, 0, 0, 0,
};

static void lifted_entry(Machine *m)
{
    AX = LIFTED_AX;
    lift_ret_far(m, 0);
}

static LiftedEntry entries[1];
static LiftedImage image;
static bool probe_before;

static Machine *new_machine(void)
{
    Machine *m = machine_create();
    m->cpu.flags &= ~F_IF;
    entries[0] = (LiftedEntry){ .module = "TEST.OVL", .off = ENTRY_OFF, .fn = lifted_entry, .name = "entry",
                                .scratch_regs = LIFT_REG_FLAGS };
    memcpy(entries[0].signature, IMAGE + ENTRY_OFF, LIFT_SIGNATURE_LEN);
    image = (LiftedImage){ .module = "TEST.OVL", .bytes = sizeof IMAGE, .head_mask = 0xFFFFFFFFu };
    memcpy(image.head, IMAGE, LIFT_IMAGE_HEAD_LEN);
    image.head_mask &= ~(3u << 14); /* two bytes the loader would relocate */
    lift_add_table(m, entries, 1);
    probe_before = m->far_call_probe; /* a lifted build registers its own images */
    lift_add_image(m, &image);
    CHECK(m->far_call_probe, "an image waiting turns the probe on");
    lift_module_loaded(m, "TEST.EXE", PROGRAM_SEG, 0x100);
    return m;
}

static void place_image(Machine *m, uint16_t seg)
{
    memcpy(m->mem + real_addr(seg, 0), IMAGE, sizeof IMAGE);
    mem_wr16(m, real_addr(seg, 14), seg); /* the relocated word holds anything */
}

/* The program far-calls seg:off (CALLF direct, or indirect through a pointer) and halts; returns AX. */
static uint16_t program_calls(Machine *m, uint16_t seg, uint16_t off, bool indirect)
{
    static const uint8_t indirect_call[] = { 0xFF, 0x1E, 0x40, 0x00, 0xF4 }; /* call far [0040] / hlt */
    const uint8_t direct_call[] = { 0x9A, (uint8_t)off, (uint8_t)(off >> 8), (uint8_t)seg, (uint8_t)(seg >> 8), 0xF4 };
    if (indirect) {
        memcpy(m->mem + real_addr(PROGRAM_SEG, 0), indirect_call, sizeof indirect_call);
        mem_wr16(m, real_addr(PROGRAM_SEG, 0x40), off);
        mem_wr16(m, real_addr(PROGRAM_SEG, 0x42), seg);
    } else {
        memcpy(m->mem + real_addr(PROGRAM_SEG, 0), direct_call, sizeof direct_call);
    }
    cpu_set_seg(&m->cpu, S_CS, PROGRAM_SEG);
    cpu_set_seg(&m->cpu, S_DS, PROGRAM_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = STACK_TOP;
    m->cpu.eip = 0;
    m->cpu.halted = false;
    AX = 0;
    for (int i = 0; i < 100 && !m->cpu.halted; i++)
        machine_step(m);
    CHECK(m->cpu.halted && SP == STACK_TOP, "the call returned");
    return AX;
}

static void test_found_by_far_calls(void)
{
    Machine *m = new_machine();
    place_image(m, IMAGE_A);
    CHECK(program_calls(m, IMAGE_A, ENTRY_OFF, false) == ORIGINAL_AX, "a call past offset 0 finds nothing");
    CHECK(lift_module_base(m, "TEST.OVL") == 0, "still not announced");

    mem_wr8(m, real_addr(IMAGE_A, 3), 'X');
    CHECK(program_calls(m, IMAGE_A, 0, false) == ORIGINAL_AX, "other head bytes run the original");
    CHECK(lift_module_base(m, "TEST.OVL") == 0, "and announce nothing");

    place_image(m, IMAGE_A);
    CHECK(program_calls(m, IMAGE_A, 0, false) == LIFTED_AX, "the first call at offset 0 arms the entries");
    CHECK(lift_module_base(m, "TEST.OVL") == IMAGE_A, "announced where it was called");
    CHECK(m->far_call_probe == probe_before, "the image no longer waits: the probe is as before");

    lift_module_unloaded(m, IMAGE_A, 0x10);
    CHECK(m->far_call_probe, "freeing its memory makes it wait again");
    CHECK(program_calls(m, IMAGE_A, ENTRY_OFF, false) == ORIGINAL_AX, "its hooks went with it");
    place_image(m, IMAGE_B);
    CHECK(program_calls(m, IMAGE_B, 0, true) == LIFTED_AX, "an indirect far call finds the next load");
    CHECK(lift_module_base(m, "TEST.OVL") == IMAGE_B, "announced at the new place");
    machine_destroy(m);
}

static void test_not_at_module_starts(void)
{
    Machine *m = new_machine();
    place_image(m, IMAGE_A);
    lift_module_loaded(m, "OTHER.DRV", IMAGE_A, sizeof IMAGE);
    CHECK(program_calls(m, IMAGE_A, 0, false) == ORIGINAL_AX, "offset 0 of an announced module is that module's");
    CHECK(lift_module_base(m, "TEST.OVL") == 0, "so the image is not announced there");
    machine_destroy(m);
}

/* What a lifted loader does: it calls the image's start through guest_call_far. */
static void test_found_by_guest_calls(void)
{
    Machine *m = new_machine();
    place_image(m, IMAGE_C);
    cpu_set_seg(&m->cpu, S_CS, PROGRAM_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = STACK_TOP;
    m->cpu.eip = 0x80;
    guest_call_far(m, IMAGE_C, 0);
    CHECK(AX == LIFTED_AX, "guest_call_far armed the image before running it (AX=%04X)", AX);
    CHECK(lift_module_base(m, "TEST.OVL") == IMAGE_C && SP == STACK_TOP, "announced, stack balanced");
    machine_destroy(m);
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : 0;
}

static void test_dump_point(void)
{
    const char *dir = "test_lift_image.out";
    mkdir(dir, 0777);
    setenv("INN_DUMP_AT", "TEST.EXE:2000:0010:2", 1);
    setenv("INN_DUMP_DIR", dir, 1);
    Machine *m = new_machine();
    CHECK(m->far_call_probe, "a module address point turns the probe on");
    place_image(m, PROGRAM_SEG + 0x2000);
    char path[256];
    snprintf(path, sizeof path, "%s/TEST.EXE:2000:0010-2.bin", dir);
    unlink(path);
    program_calls(m, PROGRAM_SEG + 0x2000, ENTRY_OFF, false);
    CHECK(file_size(path) == 0, "the first call is not the second");
    program_calls(m, PROGRAM_SEG + 0x2000, ENTRY_OFF, true);
    CHECK(file_size(path) == 0x10FFF0, "the second far call there dumps memory");
    CHECK(m->mem[real_addr(PROGRAM_SEG + 0x2000, ENTRY_OFF)] == 0xB8, "image in place");
    unlink(path);
    snprintf(path, sizeof path, "%s/TEST.EXE:2000:0010-2.txt", dir);
    unlink(path);
    rmdir(dir);
    unsetenv("INN_DUMP_AT");
    unsetenv("INN_DUMP_DIR");
    machine_destroy(m);
}

int main(void)
{
    test_found_by_far_calls();
    test_not_at_module_starts();
    test_found_by_guest_calls();
    test_dump_point();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures != 0;
}
