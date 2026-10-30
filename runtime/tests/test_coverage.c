/* Call coverage: call and interrupt targets named per module, overlays by stub, counts merged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

enum { LOAD_SEG = 0x1000, CODE_SEG = 0x1001, STACK_SEG = 0x2000, OVL_SEG = 0x1005 };

/* At CODE_SEG (module segment 1): main: call far F / call far F / int 60h / call far O / hlt.
 * F: call G / retf.  G: ret.  H (int 60h): iret.  O (an overlay at OVL_SEG:0): retf. */
static const uint8_t PROGRAM[] = {
    [0x00] = 0x9A, 0x14, 0x00, CODE_SEG & 0xFF, CODE_SEG >> 8,
    [0x05] = 0x9A, 0x14, 0x00, CODE_SEG & 0xFF, CODE_SEG >> 8,
    [0x0A] = 0xCD, 0x60, 0x9A, 0x00, 0x00, OVL_SEG & 0xFF, OVL_SEG >> 8, 0xF4,
    [0x14] = 0xE8, 0x09, 0x00, 0xCB,
    [0x20] = 0xC3, [0x22] = 0xCB, [0x24] = 0xCF,
};

static char *slurp(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[n] = 0;
    if (f)
        fclose(f);
    return buf;
}

int main(void)
{
    char path[] = "/tmp/inn_coverage_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "temporary file");
    close(fd);
    setenv("INN_COVERAGE", path, 1);
    Machine *m = machine_create();
    unsetenv("INN_COVERAGE");
    CHECK(m->coverage != NULL, "INN_COVERAGE enables it");
    memcpy(m->mem + real_addr(CODE_SEG, 0), PROGRAM, sizeof PROGRAM);
    memcpy(m->mem + real_addr(OVL_SEG, 0), PROGRAM + 0x22, 1);
    mem_wr16(m, 0x60 * 4, 0x24);
    mem_wr16(m, 0x60 * 4 + 2, CODE_SEG);
    lift_module_loaded(m, "TEST.EXE", LOAD_SEG, 0x1000);
    lift_overlay_loaded(m, "TEST.EXE#0042", OVL_SEG, 16, LOAD_SEG);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    SP = 0x100;
    m->cpu.eip = 0;
    m->cpu.flags &= ~F_IF;
    for (int i = 0; i < 100 && !m->cpu.halted; i++)
        machine_step(m);
    CHECK(m->cpu.halted, "program reached hlt");
    /* A module unloaded and reloaded: the same targets merge into one row each. */
    lift_module_unloaded(m, LOAD_SEG, 0x100);
    lift_module_loaded(m, "TEST.EXE", LOAD_SEG, 0x1000);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    m->cpu.eip = 0x14;
    lift_coverage_hit(m, LIFT_COVERAGE_CALL);
    machine_destroy(m);

    const char *want = "TEST.EXE:0001:0014\t3\tcall\n"
                       "TEST.EXE:0001:0020\t2\tcall\n"
                       "TEST.EXE:0001:0024\t1\tint\n"
                       "TEST.EXE#0042:0000\t1\tcall\n";
    const char *got = slurp(path);
    const char *body = strchr(got, '\n');
    CHECK(got[0] == '#' && body && !strcmp(body + 1, want), "coverage file:\n%s", got);
    unlink(path);

    /* An overlay read over part of another evicts it: a target in the overlap belongs to the newcomer even
     * when the stale extent is the smaller one. */
    char path2[] = "/tmp/inn_coverage_XXXXXX";
    fd = mkstemp(path2);
    CHECK(fd >= 0, "temporary file");
    close(fd);
    setenv("INN_COVERAGE", path2, 1);
    m = machine_create();
    unsetenv("INN_COVERAGE");
    lift_module_loaded(m, "TEST.EXE", LOAD_SEG, 0x1000);
    lift_overlay_loaded(m, "TEST.EXE#0043", 0x1100, 0x100, LOAD_SEG);
    lift_overlay_loaded(m, "TEST.EXE#0044", 0x10F0, 0x180, LOAD_SEG);
    cpu_set_seg(&m->cpu, S_CS, 0x1100);
    m->cpu.eip = 0x10;
    lift_coverage_hit(m, LIFT_COVERAGE_CALL);
    machine_destroy(m);
    got = slurp(path2);
    body = strchr(got, '\n');
    CHECK(got[0] == '#' && body && !strcmp(body + 1, "TEST.EXE#0044:0110\t1\tcall\n"), "evicted overlay:\n%s", got);
    unlink(path2);
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures != 0;
}
