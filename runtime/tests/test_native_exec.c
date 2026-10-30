/* native_exec and native_run_top (runtime/src/native/native_exec.c): a child that runs on the emulator,
 * a child whose start is a lifted entry (native), nesting, the native-only arming rule, memory dumps. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dos_int.h"
#include "lift_internal.h"
#include "native/native_exec.h"

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

enum { COM_SIZE = 0x40, MARKER_AX = 0x1234, MARKER_CX = 0x5678, MARKER_SP_TOP = 0xCAFE, MAX_BLOCKS = 64 };

/* mov ax, 4C00h + code / int 21h: a .COM that exits with code. */
static void exit_com(uint8_t *file, uint8_t code)
{
    static const uint8_t TEMPLATE[] = { 0xB8, 0x00, 0x4C, 0xCD, 0x21 };
    memset(file, 0x90, COM_SIZE);
    memcpy(file, TEMPLATE, sizeof TEMPLATE);
    file[1] = code;
}

/* ---- scratch drive ---- */

static char root[1024];

static void remove_tree(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            remove_tree(path);
        else
            unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

static void make_root(const char *name)
{
    const char *src = __FILE__;
    const char *tail = strstr(src, "runtime/tests/");
    char base[900];
    if (getenv("DOSRT_TMP"))
        snprintf(base, sizeof base, "%s", getenv("DOSRT_TMP"));
    else if (tail && src[0] == '/')
        snprintf(base, sizeof base, "%.*swork/tmp", (int)(tail - src), src);
    else
        snprintf(base, sizeof base, "work/tmp");
    mkdir(base, 0777);
    snprintf(root, sizeof root, "%s/test_nexec_%d_%s", base, (int)getpid(), name);
    remove_tree(root);
    if (mkdir(root, 0777) != 0) {
        perror(root);
        exit(1);
    }
}

static void write_host(const char *rel, const void *data, size_t len)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fwrite(data, 1, len, f);
    fclose(f);
}

static void write_exit_com(const char *rel, uint8_t code)
{
    uint8_t file[COM_SIZE];
    exit_com(file, code);
    write_host(rel, file, sizeof file);
}

/* ---- state to compare before and after ---- */

/* The blocks in use and the total free paragraphs: DOS does not merge neighbouring free blocks when a
 * program ends, so a freed child may leave the free space split. */
typedef struct Chain {
    int used;
    uint16_t seg[MAX_BLOCKS], owner[MAX_BLOCKS], paras[MAX_BLOCKS];
    unsigned free_paras;
} Chain;

static Chain read_chain(Machine *m)
{
    Chain c = { 0 };
    uint16_t mcb = mem_rd16(m, real_addr(DOS_DATA_SEG, DD_FIRST_MCB));
    for (int guard = 0; guard < 4 * MAX_BLOCKS; guard++) {
        uint8_t type = mem_rd8(m, real_addr(mcb, MCB_TYPE));
        if (type != 'M' && type != 'Z')
            break;
        uint16_t owner = mem_rd16(m, real_addr(mcb, MCB_OWNER)), paras = mem_rd16(m, real_addr(mcb, MCB_SIZE));
        if (owner == 0) {
            c.free_paras += (unsigned)paras + 1;
        } else if (c.used < MAX_BLOCKS) {
            c.seg[c.used] = mcb;
            c.owner[c.used] = owner;
            c.paras[c.used] = paras;
            c.used++;
        }
        if (type == 'Z')
            break;
        mcb = (uint16_t)(mcb + paras + 1);
    }
    return c;
}

static bool same_chain(const Chain *a, const Chain *b)
{
    return a->used == b->used && a->free_paras == b->free_paras && !memcmp(a->seg, b->seg, sizeof a->seg) &&
           !memcmp(a->owner, b->owner, sizeof a->owner) && !memcmp(a->paras, b->paras, sizeof a->paras);
}

static bool chain_unchanged(Machine *m, const Chain *before)
{
    Chain now = read_chain(m);
    return same_chain(before, &now);
}

static Machine *new_machine(void)
{
    Machine *m = machine_create();
    dos_init(m, root);
    return m;
}

static void finish(Machine *m)
{
    dos_shutdown(m);
    machine_destroy(m);
    remove_tree(root);
}

/* Sets the registers and the stack top the caller of native_exec expects back. */
static void set_caller_state(Machine *m)
{
    AX = MARKER_AX;
    CX = MARKER_CX;
    cpu_set_seg(&m->cpu, S_DS, 0x0777);
    mem_wr16(m, real_addr(SS, SP), MARKER_SP_TOP);
}

static void check_caller_state(Machine *m)
{
    CHECK(AX == MARKER_AX && CX == MARKER_CX);
    CHECK(DS == 0x0777);
    CHECK(mem_rd16(m, real_addr(SS, SP)) == MARKER_SP_TOP);
}

/* ---- a native child: its start is a lifted entry ---- */

static int lifted_runs;
static uint8_t lifted_exit_code;

static void lifted_child(Machine *m)
{
    lifted_runs++;
    AX = (uint16_t)(0x4C00 | lifted_exit_code);
    guest_int(m, 0x21); /* terminates: unwinds past this function */
    lifted_runs += 100; /* never reached */
}

static int inner_code;

/* Shrinks itself, starts EXIT3.COM from C as an EXEC wrapper would, then exits with its code plus one. */
static void lifted_parent(Machine *m)
{
    static const uint8_t tail[2] = { 0, '\r' };
    uint16_t err = 0;
    lifted_runs++;
    ES = DS; /* a .COM owns all memory; give most of it back as TSNEXEC does before it starts programs */
    BX = 0x100;
    AX = 0x4A00;
    guest_int(m, 0x21);
    inner_code = native_exec(m, "C:\\EXIT3.COM", tail, 0, &err);
    AX = (uint16_t)(0x4C00 | (inner_code + 1));
    guest_int(m, 0x21);
}

static LiftedEntry table[1];

static void register_native_child(Machine *m, const char *module, LiftedFn fn, bool native_only)
{
    uint8_t file[COM_SIZE];
    exit_com(file, 0);
    table[0] = (LiftedEntry){ .module = module, .seg = 0, .off = 0, .fn = fn, .name = "child",
                              .scratch_regs = LIFT_REG_AX, .native_only = native_only };
    memcpy(table[0].signature, file, LIFT_SIGNATURE_LEN);
    lift_add_table(m, table, 1);
}

/* ---- tests ---- */

static void test_emulated_child(void)
{
    make_root("emulated");
    write_exit_com("EXIT3.COM", 3);
    Machine *m = new_machine();
    static const uint8_t tail[2] = { 0, '\r' };
    Chain before = read_chain(m);
    uint16_t cur = dos_state(m)->cur_psp;
    uint32_t vec22 = mem_rd32(m, 0x22 * 4);
    LiftState *s = lift_state(m);
    s->c_depth = 2; /* as if called from lifted C */
    set_caller_state(m);
    uint16_t err = 0;
    CHECK(native_exec(m, "C:\\EXIT3.COM", tail, 0, &err) == 3);
    CHECK(s->c_depth == 2 && s->nesting == 0);
    CHECK(chain_unchanged(m, &before));
    CHECK(dos_state(m)->cur_psp == cur && dos_state(m)->depth == 0);
    CHECK(mem_rd32(m, 0x22 * 4) == vec22);
    CHECK(!m->exit_requested);
    check_caller_state(m);
    s->c_depth = 0;
    CHECK(native_exec(m, "C:\\NOPE.COM", tail, 0, &err) == -1 && err == DE_FILE_NOT_FOUND);
    check_caller_state(m);
    CHECK(chain_unchanged(m, &before));
    finish(m);
}

static void test_native_child(void)
{
    make_root("native");
    write_exit_com("CHILD.COM", 0);
    Machine *m = machine_create();
    register_native_child(m, "CHILD.COM", lifted_child, false);
    dos_init(m, root);
    static const uint8_t tail[2] = { 0, '\r' };
    Chain before = read_chain(m);
    uint16_t cur = dos_state(m)->cur_psp;
    LiftState *s = lift_state(m);
    set_caller_state(m);
    lifted_runs = 0;
    lifted_exit_code = 5;
    uint64_t insns = m->cpu.instructions;
    uint16_t err = 0;
    CHECK(native_exec(m, "C:\\CHILD.COM", tail, 0, &err) == 5);
    CHECK(lifted_runs == 1);
    CHECK(m->cpu.instructions == insns); /* no guest instruction ran */
    CHECK(s->c_depth == 0 && s->nesting == 0);
    CHECK(chain_unchanged(m, &before));
    CHECK(dos_state(m)->cur_psp == cur && dos_state(m)->depth == 0);
    check_caller_state(m);
    /* The same machine runs it again and gets the new code. */
    lifted_exit_code = 9;
    CHECK(native_exec(m, "C:\\CHILD.COM", tail, 0, &err) == 9 && lifted_runs == 2);
    check_caller_state(m);
    finish(m);
}

static void test_nested(void)
{
    make_root("nested");
    write_exit_com("PARENT.COM", 0);
    write_exit_com("EXIT3.COM", 3);
    Machine *m = machine_create();
    register_native_child(m, "PARENT.COM", lifted_parent, false);
    dos_init(m, root);
    static const uint8_t tail[2] = { 0, '\r' };
    Chain before = read_chain(m);
    set_caller_state(m);
    lifted_runs = 0;
    inner_code = -1;
    uint16_t err = 0;
    CHECK(native_exec(m, "C:\\PARENT.COM", tail, 0, &err) == 4);
    CHECK(inner_code == 3 && lifted_runs == 1);
    CHECK(lift_state(m)->nesting == 0 && lift_state(m)->c_depth == 0);
    CHECK(chain_unchanged(m, &before));
    check_caller_state(m);
    finish(m);
}

/* An emulated child whose lifted code ends the process from a nested guest call, as a game's lifted
 * exit routine does: the C frame is left (as for a native child) instead of running the caller's code. */
enum { NESTED_ENTRY = 0x10, NESTED_EXIT = 0x20, NESTED_CODE = 3 };

static int nested_exit_after;

static void lifted_nested_exit(Machine *m)
{
    guest_call_near(m, 0x100 + NESTED_EXIT); /* exits: never returns here */
    nested_exit_after++;
}

static void test_exit_in_nested_call(void)
{
    make_root("nested_exit");
    uint8_t file[COM_SIZE];
    memset(file, 0x90, sizeof file);
    static const uint8_t start[] = { 0xE8, NESTED_ENTRY - 3, 0x00, 0xB8, 0x01, 0x4C, 0xCD, 0x21 }; /* call; exit 1 */
    static const uint8_t entry[] = { 0xB8, 0x02, 0x4C, 0xCD, 0x21 };                            /* exit 2 */
    static const uint8_t done[] = { 0xB8, NESTED_CODE, 0x4C, 0xCD, 0x21 };                       /* exit 3 */
    memcpy(file, start, sizeof start);
    memcpy(file + NESTED_ENTRY, entry, sizeof entry);
    memcpy(file + NESTED_EXIT, done, sizeof done);
    write_host("CHILD.COM", file, sizeof file);
    Machine *m = machine_create();
    table[0] = (LiftedEntry){ .module = "CHILD.COM", .seg = 0, .off = NESTED_ENTRY, .fn = lifted_nested_exit,
                              .name = "nested_exit", .near_return = true, .scratch_regs = LIFT_REG_AX };
    memcpy(table[0].signature, file + NESTED_ENTRY, LIFT_SIGNATURE_LEN);
    lift_add_table(m, table, 1);
    dos_init(m, root);
    static const uint8_t tail[2] = { 0, '\r' };
    Chain before = read_chain(m);
    uint16_t cur = dos_state(m)->cur_psp;
    LiftState *s = lift_state(m);
    s->c_depth = 1;
    set_caller_state(m);
    nested_exit_after = 0;
    s->nested_budget = m->cpu.instructions + 100000; /* without the unwind the caller's code would run on */
    uint16_t err = 0;
    CHECK(native_exec(m, "C:\\CHILD.COM", tail, 0, &err) == NESTED_CODE);
    s->nested_budget = 0;
    CHECK(nested_exit_after == 0);
    CHECK(!s->nested_abort);
    CHECK(s->c_depth == 1 && s->nesting == 0);
    CHECK(chain_unchanged(m, &before));
    CHECK(dos_state(m)->cur_psp == cur && dos_state(m)->depth == 0);
    check_caller_state(m);
    s->c_depth = 0;
    finish(m);
}

static void test_run_top(void)
{
    make_root("top");
    write_exit_com("EXIT3.COM", 3);
    write_exit_com("CHILD.COM", 0);
    Machine *m = new_machine();
    CHECK(dos_start_program(m, "C:\\EXIT3.COM", NULL));
    native_run_top(m);
    CHECK(m->exit_requested && m->exit_code == 3);
    CHECK(lift_state(m)->nesting == 0);
    finish(m);

    make_root("top_native");
    write_exit_com("CHILD.COM", 0);
    m = machine_create();
    register_native_child(m, "CHILD.COM", lifted_child, false);
    dos_init(m, root);
    lifted_runs = 0;
    lifted_exit_code = 7;
    CHECK(dos_start_program(m, "C:\\CHILD.COM", NULL));
    native_run_top(m);
    CHECK(m->exit_requested && m->exit_code == 7 && lifted_runs == 1);
    finish(m);
}

/* Native-only entries are armed with m->native set, or with INN_NATIVE_CONTROL=1, not otherwise. */
static bool armed_after_load(bool native, const char *control)
{
    if (control)
        setenv("INN_NATIVE_CONTROL", control, 1);
    else
        unsetenv("INN_NATIVE_CONTROL");
    Machine *m = machine_create();
    m->native = native;
    register_native_child(m, "CHILD.COM", lifted_child, true);
    lift_module_loaded(m, "CHILD.COM", 0x2000, COM_SIZE);
    bool armed = lift_hooked(m, real_addr(0x2000, 0));
    unsetenv("INN_NATIVE_CONTROL");
    machine_destroy(m);
    return armed;
}

static void test_arming(void)
{
    CHECK(!armed_after_load(false, NULL));
    CHECK(!armed_after_load(false, "0"));
    CHECK(armed_after_load(true, NULL));
    CHECK(armed_after_load(false, "1"));
    /* An entry that is not native-only is always armed. */
    Machine *m = machine_create();
    register_native_child(m, "CHILD.COM", lifted_child, false);
    lift_module_loaded(m, "CHILD.COM", 0x2000, COM_SIZE);
    CHECK(lift_hooked(m, real_addr(0x2000, 0)));
    machine_destroy(m);
}

static size_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (size_t)st.st_size : 0;
}

static bool file_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    bool found = false;
    while (!found && fgets(line, sizeof line, f))
        found = strstr(line, needle) != NULL;
    fclose(f);
    return found;
}

static void test_dumps(void)
{
    make_root("dump");
    write_exit_com("EXIT3.COM", 3);
    char dir[1100], path[1200];
    snprintf(dir, sizeof dir, "%s/out", root);
    mkdir(dir, 0777);
    setenv("INN_DUMP_AT", "exec:2,start:1", 1);
    setenv("INN_DUMP_DIR", dir, 1);
    Machine *m = new_machine();
    static const uint8_t tail[2] = { 0, '\r' };
    uint16_t err = 0;
    CHECK(native_exec(m, "C:\\EXIT3.COM", tail, 0, &err) == 3);
    snprintf(path, sizeof path, "%s/exec-2.bin", dir);
    CHECK(file_size(path) == 0); /* only the first exec so far */
    CHECK(native_exec(m, "C:\\EXIT3.COM", tail, 0, &err) == 3);
    snprintf(path, sizeof path, "%s/exec-2.bin", dir);
    CHECK(file_size(path) == 0x10FFF0);
    snprintf(path, sizeof path, "%s/exec-2.txt", dir);
    CHECK(file_has(path, "[mcb]") && file_has(path, "[ivt]") && file_has(path, "21 F000:"));
    CHECK(file_has(path, "[sft]") && file_has(path, "cwd=C:"));
    snprintf(path, sizeof path, "%s/start-1.txt", dir);
    CHECK(file_has(path, "EXIT3")); /* the child's block exists when it starts */
    snprintf(path, sizeof path, "%s/start-1.bin", dir);
    CHECK(file_size(path) == 0x10FFF0);
    snprintf(path, sizeof path, "%s/exec-1.bin", dir);
    CHECK(file_size(path) == 0);
    unsetenv("INN_DUMP_AT");
    unsetenv("INN_DUMP_DIR");
    finish(m);
}

int main(void)
{
    test_emulated_child();
    test_native_child();
    test_nested();
    test_exit_in_nested_call();
    test_run_top();
    test_arming();
    test_dumps();
    if (failures)
        fprintf(stderr, "test_native_exec: %d failure(s)\n", failures);
    else
        fprintf(stderr, "test_native_exec: all passed\n");
    return failures ? 1 : 0;
}
