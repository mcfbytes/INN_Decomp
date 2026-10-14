/* DOS layer tests: tiny .COM/.EXE programs, assembled here byte by byte, exercise files,
 * directories, search, memory blocks and EXEC/terminate/TSR on a scratch C: drive. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "borland_overlay.h"
#include "dos_int.h"
#include "lift.h"
#include "machine.h"

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

/* ---- a minimal 8086 emitter ---- */

enum { COM_ORG = 0x100, PROG_SIZE = 0x1000, DATA = 0x900, STACK_TOP = 0x1FFE, KEEP_PARAS = 0x200 };
enum { SR_ES = 0, SR_CS = 1, SR_SS = 2, SR_DS = 3 };

typedef struct Prog {
    uint8_t b[PROG_SIZE];
    uint16_t pc;  /* emission offset in b */
    uint16_t org; /* address of b[0] */
} Prog;

static void e8(Prog *p, unsigned v) { p->b[p->pc++] = (uint8_t)v; }
static void e16(Prog *p, unsigned v) { e8(p, v & 0xFF); e8(p, v >> 8 & 0xFF); }
static uint16_t here(const Prog *p) { return (uint16_t)(p->org + p->pc); }
static void mov_imm(Prog *p, int reg, unsigned v) { e8(p, 0xB8 + reg); e16(p, v); }
static void mov_rr(Prog *p, int dst, int src) { e8(p, 0x89); e8(p, 0xC0 | src << 3 | dst); }
static void mov_sreg(Prog *p, int sreg, int reg) { e8(p, 0x8E); e8(p, 0xC0 | sreg << 3 | reg); }
static void mov_from_sreg(Prog *p, int reg, int sreg) { e8(p, 0x8C); e8(p, 0xC0 | sreg << 3 | reg); }
static void store_sreg(Prog *p, unsigned addr, int sreg) { e8(p, 0x8C); e8(p, 0x06 | sreg << 3); e16(p, addr); }
static void load_es(Prog *p, unsigned addr) { e8(p, 0x8E); e8(p, 0x06); e16(p, addr); }
static void store_ax(Prog *p, unsigned addr) { e8(p, 0xA3); e16(p, addr); }
static void load_bx(Prog *p, unsigned addr) { e8(p, 0x8B); e8(p, 0x1E); e16(p, addr); }
static void int21(Prog *p) { e8(p, 0xCD); e8(p, 0x21); }
static void dos(Prog *p, unsigned ax) { mov_imm(p, R_AX, ax); int21(p); }
static void exit_with(Prog *p, unsigned code) { e8(p, 0xB4); e8(p, 0x4C); e8(p, 0xB0); e8(p, code); int21(p); }

enum { EXIT_LEN = 6, OP_JC = 0x72, OP_JNC = 0x73, OP_JE = 0x74 };

/* Exit with step as the return code unless the previous call cleared carry. */
static void ok(Prog *p, unsigned step) { e8(p, OP_JNC); e8(p, EXIT_LEN); exit_with(p, step); }
static void fails(Prog *p, unsigned step) { e8(p, OP_JC); e8(p, EXIT_LEN); exit_with(p, step); }

static void expect_reg(Prog *p, int reg, unsigned v, unsigned step)
{
    e8(p, 0x81);
    e8(p, 0xF8 | reg); /* cmp reg, imm16 */
    e16(p, v);
    e8(p, OP_JE);
    e8(p, EXIT_LEN);
    exit_with(p, step);
}

static void jnc_back(Prog *p, uint16_t target)
{
    e8(p, OP_JNC);
    e8(p, (unsigned)(target - (here(p) + 1)) & 0xFF);
}

static void put(Prog *p, unsigned addr, const void *data, size_t len)
{
    memcpy(p->b + (addr - p->org), data, len);
}

static void put_str(Prog *p, unsigned addr, const char *s) { put(p, addr, s, strlen(s) + 1); }

/* ---- scratch drive ---- */

static char root[1024];

static void path_in_root(char *out, size_t cap, const char *rel) { snprintf(out, cap, "%s/%s", root, rel); }

static void write_host(const char *rel, const void *data, size_t len)
{
    char path[1200];
    path_in_root(path, sizeof path, rel);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fwrite(data, 1, len, f);
    fclose(f);
}

static size_t read_host(const char *rel, char *buf, size_t cap)
{
    char path[1200];
    path_in_root(path, sizeof path, rel);
    FILE *f = fopen(path, "rb");
    if (!f)
        return (size_t)-1;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    return n;
}

static void write_com(const char *rel, const Prog *p)
{
    if (p->pc > DATA - COM_ORG) {
        fprintf(stderr, "%s: code overlaps data\n", rel);
        exit(1);
    }
    write_host(rel, p->b, PROG_SIZE);
}

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
    snprintf(root, sizeof root, "%s/test_dos_%d_%s", base, (int)getpid(), name);
    remove_tree(root);
    if (mkdir(root, 0777) != 0) {
        perror(root);
        exit(1);
    }
}

static Machine *new_machine(void)
{
    Machine *m = machine_create();
    m->trace = getenv("DOS_TRACE") ? 1 : 0;
    dos_init(m, root);
    return m;
}

static int run(Machine *m)
{
    for (int i = 0; i < 200 && !m->exit_requested; i++)
        cpu_run(m, 100000);
    return m->exit_requested ? m->exit_code : -1;
}

static void finish(Machine *m)
{
    dos_shutdown(m);
    machine_destroy(m);
    remove_tree(root);
}

/* ---- MCB chain inspection ---- */

typedef struct ChainInfo {
    int blocks, used;
    bool ends_ok;
    uint16_t owner_size[4][2]; /* first few used blocks: owner, size */
} ChainInfo;

static ChainInfo walk_chain(Machine *m)
{
    ChainInfo ci = {0};
    uint16_t mcb = mem_rd16(m, real_addr(DOS_DATA_SEG, DD_FIRST_MCB));
    for (int guard = 0; guard < 1000; guard++) {
        uint8_t type = mem_rd8(m, real_addr(mcb, 0));
        uint16_t owner = mem_rd16(m, real_addr(mcb, 1)), size = mem_rd16(m, real_addr(mcb, 3));
        if (type != 'M' && type != 'Z')
            break;
        ci.blocks++;
        if (owner) {
            if (ci.used < 4)
                ci.owner_size[ci.used][0] = owner, ci.owner_size[ci.used][1] = size;
            ci.used++;
        }
        if (type == 'Z') {
            ci.ends_ok = mcb + size + 1 == DOS_ARENA_END;
            break;
        }
        mcb = (uint16_t)(mcb + size + 1);
    }
    return ci;
}

/* ---- tests ---- */

enum { NAME_A = DATA, NAME_SUB = DATA + 0x10, NAME_B = DATA + 0x18, NAME_LOWER = DATA + 0x20,
       NAME_C = DATA + 0x38, NAME_DEL = DATA + 0x40, NAME_NOPE = DATA + 0x48, NAME_DOTDOT = DATA + 0x58,
       PATTERN = DATA + 0x60, NAME_E = DATA + 0x68, MSG = DATA + 0x70, HELLO = DATA + 0x80,
       HANDLE = DATA + 0x90, CWD = DATA + 0xA0, BUF = DATA + 0x100, DTA_BUF = DATA + 0x180 };

static void emit_file_basics(Prog *p)
{
    mov_imm(p, R_CX, 0); mov_imm(p, R_DX, NAME_A); dos(p, 0x3C00); ok(p, 1);
    store_ax(p, HANDLE); mov_rr(p, R_BX, R_AX);
    mov_imm(p, R_CX, 5); mov_imm(p, R_DX, HELLO); dos(p, 0x4000); ok(p, 2);
    expect_reg(p, R_AX, 5, 3);
    load_bx(p, HANDLE); mov_imm(p, R_CX, 0); mov_imm(p, R_DX, 0); dos(p, 0x4202); ok(p, 4);
    expect_reg(p, R_AX, 5, 5);
    mov_imm(p, R_DX, 0); dos(p, 0x4200); ok(p, 6);
    mov_imm(p, R_CX, 100); mov_imm(p, R_DX, BUF); dos(p, 0x3F00); ok(p, 7);
    expect_reg(p, R_AX, 5, 8);
    dos(p, 0x3E00); ok(p, 9);
}

static void emit_dirs(Prog *p)
{
    mov_imm(p, R_DX, NAME_SUB); dos(p, 0x3900); ok(p, 10);
    dos(p, 0x3B00); ok(p, 11);
    mov_imm(p, R_CX, 0); mov_imm(p, R_DX, NAME_B); dos(p, 0x3C00); ok(p, 12);
    mov_rr(p, R_BX, R_AX); mov_imm(p, R_CX, 5); mov_imm(p, R_DX, BUF); dos(p, 0x4000); ok(p, 13);
    dos(p, 0x3E00); ok(p, 14);
    mov_imm(p, R_DX, 0); mov_imm(p, R_SI, CWD); dos(p, 0x4700); ok(p, 15);
    mov_imm(p, R_DX, NAME_DOTDOT); dos(p, 0x3B00); ok(p, 16);
    mov_imm(p, R_DX, NAME_LOWER); dos(p, 0x3D00); ok(p, 17);
    mov_rr(p, R_BX, R_AX); mov_imm(p, R_CX, 1); mov_imm(p, R_DX, BUF + 0x40); dos(p, 0x3F00); ok(p, 18);
    expect_reg(p, R_AX, 1, 19);
    dos(p, 0x3E00);
}

static void emit_names(Prog *p)
{
    mov_imm(p, R_DX, NAME_A); mov_imm(p, R_DI, NAME_C); dos(p, 0x5600); ok(p, 20);
    mov_imm(p, R_DX, NAME_DEL); dos(p, 0x4100); ok(p, 21);
    mov_imm(p, R_DX, NAME_NOPE); dos(p, 0x3D00); fails(p, 22);
    expect_reg(p, R_AX, DE_FILE_NOT_FOUND, 23);
    mov_imm(p, R_BX, 1); mov_imm(p, R_CX, 12); mov_imm(p, R_DX, MSG); dos(p, 0x4000); ok(p, 24);
    mov_imm(p, R_DX, NAME_SUB); dos(p, 0x4300); ok(p, 25);
    expect_reg(p, R_CX, ATTR_DIR, 26);
    mov_imm(p, R_BX, 1); dos(p, 0x4400); ok(p, 27);
    expect_reg(p, R_DX, 0x80D3, 28);
}

static void emit_search(Prog *p)
{
    mov_imm(p, R_DX, DTA_BUF); dos(p, 0x1A00);
    mov_imm(p, R_SI, 0); mov_imm(p, R_CX, 0); mov_imm(p, R_DX, PATTERN); dos(p, 0x4E00); ok(p, 29);
    uint16_t loop = here(p);
    e8(p, 0x40 + R_SI); /* inc si */
    dos(p, 0x4F00);
    jnc_back(p, loop);
    expect_reg(p, R_AX, DE_NO_MORE_FILES, 30);
    expect_reg(p, R_SI, 2, 31);
    mov_imm(p, R_BX, 2); mov_imm(p, R_CX, 0); mov_imm(p, R_DX, 0x0010); mov_imm(p, R_SI, NAME_E);
    dos(p, 0x6C00); ok(p, 32);
    expect_reg(p, R_CX, 2, 33);
    mov_rr(p, R_BX, R_AX); dos(p, 0x3E00);
}

/* Shadow verification undoes a run's deletions and renames, then makes the last run's permanent. */
static void test_journal(void)
{
    make_root("journal");
    write_host("A.TXT", "a", 1);
    char a[1100], b[1100], buf[4];
    snprintf(a, sizeof a, "%s/A.TXT", root);
    snprintf(b, sizeof b, "%s/B.TXT", root);
    dos_journal_begin(NULL);
    CHECK(dos_host_unlink(a) == 0 && read_host("A.TXT", buf, sizeof buf) == (size_t)-1);
    dos_journal_undo(NULL);
    CHECK(read_host("A.TXT", buf, sizeof buf) == 1);
    CHECK(dos_host_rename(a, b) == 0);
    dos_journal_undo(NULL);
    CHECK(read_host("A.TXT", buf, sizeof buf) == 1 && read_host("B.TXT", buf, sizeof buf) == (size_t)-1);
    char c[1100];
    snprintf(c, sizeof c, "%s/C.TXT", root);
    int fd = dos_host_open(c, O_RDWR | O_CREAT | O_TRUNC, false);
    CHECK(fd >= 0);
    close(fd);
    fd = dos_host_open(a, O_RDWR, true);
    CHECK(fd >= 0 && write(fd, "bcd", 3) == 3 && write(fd, "e", 1) == 1);
    close(fd);
    dos_journal_undo(NULL);
    CHECK(read_host("C.TXT", buf, sizeof buf) == (size_t)-1);
    CHECK(read_host("A.TXT", buf, sizeof buf) == 1 && buf[0] == 'b'); /* in-place bytes stay */
    fd = dos_host_open(a, O_RDWR | O_CREAT | O_TRUNC, true);
    CHECK(fd >= 0);
    close(fd);
    dos_journal_undo(NULL);
    CHECK(read_host("A.TXT", buf, sizeof buf) == 1);
    CHECK(dos_host_unlink(a) == 0);
    dos_journal_end(NULL);
    CHECK(read_host("A.TXT", buf, sizeof buf) == (size_t)-1);
    int left = 0;
    DIR *d = opendir(root);
    for (struct dirent *e; (e = readdir(d));)
        left += e->d_name[0] != '.';
    closedir(d);
    CHECK(left == 0);
}

static void test_files(void)
{
    make_root("files");
    write_host("lower.dat", "Z", 1);
    write_host("two.txt", "2", 1);
    write_host("DEL.TXT", "x", 1);
    Prog p = {.org = COM_ORG};
    emit_file_basics(&p);
    emit_dirs(&p);
    emit_names(&p);
    emit_search(&p);
    exit_with(&p, 0);
    put_str(&p, NAME_A, "a.txt");
    put_str(&p, NAME_SUB, "Sub");
    put_str(&p, NAME_B, "B.TXT");
    put_str(&p, NAME_LOWER, "sub/../LOWER.DAT");
    put_str(&p, NAME_C, "C:\\C.TXT");
    put_str(&p, NAME_DEL, "del.txt");
    put_str(&p, NAME_NOPE, "NOPE.TXT");
    put_str(&p, NAME_DOTDOT, "..");
    put_str(&p, PATTERN, "*.TXT");
    put_str(&p, NAME_E, "E.TXT");
    put_str(&p, MSG, "hello, dos\r\n");
    put_str(&p, HELLO, "HELLO");
    write_com("FILES.COM", &p);

    Machine *m = new_machine();
    CHECK(dos_start_program(m, "C:\\FILES", ""));
    uint16_t psp = dos_state(m)->cur_psp;
    CHECK(run(m) == 0);
    char buf[64];
    CHECK(read_host("C.TXT", buf, sizeof buf) == 5 && !strcmp(buf, "HELLO"));
    CHECK(read_host("SUB/B.TXT", buf, sizeof buf) == 5 && !strcmp(buf, "HELLO"));
    CHECK(read_host("A.TXT", buf, sizeof buf) == (size_t)-1);
    CHECK(read_host("DEL.TXT", buf, sizeof buf) == (size_t)-1);
    CHECK(read_host("E.TXT", buf, sizeof buf) == 0);
    mem_read_cstr(m, psp, CWD, buf, sizeof buf);
    CHECK(!strcmp(buf, "SUB"));
    CHECK(mem_rd8(m, real_addr(psp, BUF + 0x40)) == 'Z');
    mem_read_cstr(m, psp, DTA_BUF + 0x1E, buf, sizeof buf);
    CHECK(!strcmp(buf, "TWO.TXT"));
    finish(m);
}

enum { SEG1 = DATA, SEG2 = DATA + 2, LARGEST = DATA + 4 };

static void test_memory_program(void)
{
    make_root("mem");
    Prog p = {.org = COM_ORG};
    mov_imm(&p, R_SP, STACK_TOP);
    mov_imm(&p, R_BX, KEEP_PARAS); dos(&p, 0x4A00); ok(&p, 1);
    mov_imm(&p, R_BX, 0x200); dos(&p, 0x4800); ok(&p, 2); store_ax(&p, SEG1);
    mov_imm(&p, R_BX, 0xFFFF); dos(&p, 0x4800); fails(&p, 3);
    expect_reg(&p, R_AX, DE_NO_MEMORY, 4);
    e8(&p, 0x89); e8(&p, 0x1E); e16(&p, LARGEST); /* mov [LARGEST], bx */
    dos(&p, 0x4800); ok(&p, 5); store_ax(&p, SEG2);
    load_es(&p, SEG2); dos(&p, 0x4900); ok(&p, 6);
    load_es(&p, SEG1); mov_imm(&p, R_BX, 0x300); dos(&p, 0x4A00); ok(&p, 7);
    dos(&p, 0x4900); ok(&p, 8);
    mov_imm(&p, R_DI, 0x1234); mov_sreg(&p, SR_ES, R_DI); dos(&p, 0x4900); fails(&p, 9);
    expect_reg(&p, R_AX, DE_INVALID_BLOCK, 10);
    dos(&p, 0x5800); ok(&p, 11);
    expect_reg(&p, R_AX, 0, 12);
    mov_imm(&p, R_BX, 1); dos(&p, 0x5803); fails(&p, 13);
    exit_with(&p, 0);
    write_com("MEM.COM", &p);

    Machine *m = new_machine();
    ChainInfo before = walk_chain(m);
    CHECK(dos_start_program(m, "MEM.COM", NULL));
    uint16_t psp = dos_state(m)->cur_psp;
    CHECK(run(m) == 0);
    uint16_t largest = mem_rd16(m, real_addr(psp, LARGEST));
    CHECK(largest > 0x8000 && largest < 0x9F00);
    ChainInfo after = walk_chain(m);
    CHECK(after.ends_ok);
    CHECK(after.used == before.used); /* everything the program owned is free again */
    finish(m);
}

static void test_allocator(void)
{
    make_root("alloc");
    Machine *m = new_machine();
    ChainInfo ci = walk_chain(m);
    CHECK(ci.ends_ok && ci.used == 2); /* the shell's environment and PSP */
    CHECK(mem_rd16(m, real_addr(DOS_DATA_SEG, DD_FIRST_MCB)) == DOS_FIRST_MCB);
    uint16_t a, b, c, largest;
    CHECK(dos_mem_alloc(m, 0x100, 0x777, &a, &largest) == DE_OK);
    CHECK(dos_mem_alloc(m, 0x100, 0x777, &b, &largest) == DE_OK);
    CHECK(b == a + 0x101);
    dos_state(m)->alloc_strategy = 2; /* last fit */
    CHECK(dos_mem_alloc(m, 0x10, 0x777, &c, &largest) == DE_OK);
    CHECK(c == DOS_ARENA_END - 0x10);
    dos_state(m)->alloc_strategy = 0;
    CHECK(dos_mem_free(m, a) == DE_OK);
    CHECK(dos_mem_resize(m, b, 0x50, &largest) == DE_OK);
    CHECK(dos_mem_alloc(m, 0x80, 0x777, &a, &largest) == DE_OK); /* first fit reuses the hole */
    CHECK(a < b);
    CHECK(dos_mem_resize(m, a, 0x200, &largest) == DE_NO_MEMORY);
    CHECK(largest == 0x100);
    dos_mem_free_owner(m, 0x777);
    ci = walk_chain(m);
    CHECK(ci.ends_ok && ci.used == 2);
    mem_wr8(m, real_addr((uint16_t)(b - 1), 0), 'X'); /* corrupt the arena */
    CHECK(dos_mem_alloc(m, 0x10, 0x777, &a, &largest) == DE_MCB_DESTROYED);
    finish(m);
}

/* Child: writes its command tail to TAIL.TXT using a relocated data segment; exits 42h. */
static void write_child_exe(void)
{
    enum { HDR = 0x20, IMAGE = 0x500, DATA_SEG = 0x40, RELOC_AT = 2 };
    uint8_t exe[HDR + IMAGE] = {0};
    Prog p = {.org = 0};
    e8(&p, 0x1E);                   /* push ds (PSP) */
    mov_imm(&p, R_AX, DATA_SEG);    /* relocated */
    mov_sreg(&p, SR_DS, R_AX);
    mov_imm(&p, R_CX, 0); mov_imm(&p, R_DX, 0); dos(&p, 0x3C00); ok(&p, 0x11);
    mov_rr(&p, R_BX, R_AX);
    e8(&p, 0x1F);                   /* pop ds */
    e8(&p, 0x8A); e8(&p, 0x0E); e16(&p, 0x80); /* mov cl, [80h] */
    e8(&p, 0xB5); e8(&p, 0);        /* mov ch, 0 */
    mov_imm(&p, R_DX, 0x81); e8(&p, 0xB4); e8(&p, 0x40); int21(&p); ok(&p, 0x12);
    dos(&p, 0x3E00);
    exit_with(&p, 0x42);
    memcpy(exe + HDR, p.b, p.pc);
    memcpy(exe + HDR + DATA_SEG * 16, "TAIL.TXT", 9);
    const uint16_t hdr[] = {0x5A4D, (HDR + IMAGE) % 512, (HDR + IMAGE + 511) / 512, 1, HDR / 16, 0x20,
                            0xFFFF, 0x50, 0x100, 0, 0, 0, 0x1C, 0, RELOC_AT, 0};
    for (size_t i = 0; i < sizeof hdr / sizeof hdr[0]; i++)
        exe[i * 2] = (uint8_t)hdr[i], exe[i * 2 + 1] = (uint8_t)(hdr[i] >> 8);
    write_host("CHILD.EXE", exe, sizeof exe);
}

enum { PB = DATA, OVL_PB = DATA + 0x20, TAIL = DATA + 0x30, CHILD_NAME = DATA + 0x40,
       TSR_NAME = DATA + 0x50, OVL_NAME = DATA + 0x60 };

static void emit_exec(Prog *p, unsigned name, unsigned step)
{
    mov_imm(p, R_DX, name); mov_imm(p, R_BX, PB); dos(p, 0x4B00); ok(p, step);
}

static void test_exec(void)
{
    make_root("exec");
    write_child_exe();
    const uint8_t tsr[] = {0xBA, 0x20, 0x00, 0xB8, 0x05, 0x31, 0xCD, 0x21}; /* keep 20h paras, code 5 */
    write_host("TSR.COM", tsr, sizeof tsr);
    write_host("OVL.BIN", "OVERLAY!", 8);
    Prog p = {.org = COM_ORG};
    mov_imm(&p, R_SP, STACK_TOP);
    mov_imm(&p, R_BX, KEEP_PARAS); dos(&p, 0x4A00); ok(&p, 1);
    store_sreg(&p, PB + 4, SR_CS); store_sreg(&p, PB + 8, SR_CS); store_sreg(&p, PB + 12, SR_CS);
    emit_exec(&p, TSR_NAME, 2);
    dos(&p, 0x4D00); expect_reg(&p, R_AX, 0x0305, 3);
    mov_imm(&p, R_BX, 0x10); dos(&p, 0x4800); ok(&p, 4);
    store_ax(&p, OVL_PB); store_ax(&p, OVL_PB + 2);
    mov_imm(&p, R_DX, OVL_NAME); mov_imm(&p, R_BX, OVL_PB); dos(&p, 0x4B03); ok(&p, 5);
    mov_imm(&p, R_BP, 0x1234); mov_imm(&p, R_SI, 0x5678);
    emit_exec(&p, CHILD_NAME, 6);
    expect_reg(&p, R_BP, 0x1234, 7);
    expect_reg(&p, R_SI, 0x5678, 8);
    expect_reg(&p, R_SP, STACK_TOP, 9);
    mov_from_sreg(&p, R_AX, SR_DS); mov_from_sreg(&p, R_BX, SR_CS);
    e8(&p, 0x39); e8(&p, 0xD8); e8(&p, OP_JE); e8(&p, EXIT_LEN); exit_with(&p, 10); /* cmp ax,bx */
    dos(&p, 0x4D00);
    e8(&p, 0xB4); e8(&p, 0x4C); int21(&p); /* exit with the child's code */
    const uint16_t pb_words[] = {0, TAIL, 0, 0x5C, 0, 0x6C, 0};
    put(&p, PB, pb_words, sizeof pb_words);
    put(&p, TAIL, "\x06 HELLO\r", 8);
    put_str(&p, CHILD_NAME, "CHILD.EXE");
    put_str(&p, TSR_NAME, "TSR.COM");
    put_str(&p, OVL_NAME, "OVL.BIN");
    write_com("EXEC.COM", &p);

    Machine *m = new_machine();
    CHECK(dos_start_program(m, "C:\\EXEC.COM", NULL));
    uint16_t psp = dos_state(m)->cur_psp;
    CHECK(run(m) == 0x42);
    char buf[64];
    CHECK(read_host("TAIL.TXT", buf, sizeof buf) == 6 && !strcmp(buf, " HELLO"));
    uint16_t ovl = mem_rd16(m, real_addr(psp, OVL_PB));
    char ovl_text[9] = {0};
    mem_read_block(m, real_addr(ovl, 0), ovl_text, 8);
    CHECK(!strcmp(ovl_text, "OVERLAY!"));
    ChainInfo ci = walk_chain(m);
    CHECK(ci.ends_ok);
    bool tsr_kept = false;
    for (int i = 0; i < ci.used && i < 4; i++)
        tsr_kept |= ci.owner_size[i][1] == 0x20 && ci.owner_size[i][0] != psp;
    CHECK(tsr_kept);
    CHECK(dos_state(m)->depth == 0);
    finish(m);
}

static void test_psp_and_env(void)
{
    make_root("psp");
    const uint8_t prog[] = {0xB8, 0x07, 0x4C, 0xCD, 0x21}; /* exit 7 */
    write_host("P.COM", prog, sizeof prog);
    Machine *m = new_machine();
    CHECK(dos_start_program(m, "P", "foo.txt c:bar"));
    DosState *d = dos_state(m);
    uint16_t psp = d->cur_psp;
    CHECK(mem_rd16(m, real_addr(psp, 0)) == 0x20CD);
    CHECK(mem_rd16(m, real_addr(psp, PSP_PARENT)) == d->shell_psp);
    char tail[32];
    mem_read_block(m, real_addr(psp, PSP_TAIL), tail, 16);
    CHECK(tail[0] == 14 && !memcmp(tail + 1, " foo.txt c:bar\r", 15));
    CHECK(!memcmp(mem_ptr(m, psp, PSP_FCB1), "\0FOO     TXT", 12));
    CHECK(!memcmp(mem_ptr(m, psp, PSP_FCB2), "\3BAR        ", 12));
    uint16_t env = mem_rd16(m, real_addr(psp, PSP_ENV));
    char s[128];
    mem_read_cstr(m, env, 0, s, sizeof s);
    CHECK(!strcmp(s, "COMSPEC=C:\\COMMAND.COM"));
    uint16_t off = 0;
    while (mem_rd8(m, real_addr(env, off)) || mem_rd8(m, real_addr(env, (uint16_t)(off + 1))))
        off++;
    CHECK(mem_rd16(m, real_addr(env, (uint16_t)(off + 2))) == 1);
    mem_read_cstr(m, env, (uint16_t)(off + 4), s, sizeof s);
    CHECK(!strcmp(s, "C:\\P.COM"));
    CHECK(m->cpu.seg[S_CS] == psp && m->cpu.eip == 0x100);
    uint16_t ems_seg = mem_rd16(m, 0x67 * 4 + 2);
    CHECK(memcmp(m->mem + real_addr(ems_seg, 0x0A), "EMMXXXX0", 8) != 0); /* EMS absent */
    CHECK(run(m) == 7);
    finish(m);
}


/* ---- Borland overlays: a program reads overlay code from its own EXE ---- */

enum { OVX_HDR = 0x20, OVX_IMAGE = 0x400, OVX_NAME = 0x100, OVX_TABLE = 0x200, OVX_STUB_SEG = 0x30,
       OVX_TRAILER = 16, OVX_DATA = 16, OVX_ORIGINAL = 5, OVX_LIFTED = 0x77 };

/* The overlay's code: mov ax, OVX_ORIGINAL / retf. */
static const uint8_t OVX_CODE[] = { 0xB8, OVX_ORIGINAL, 0x00, 0xCB };

static void lifted_overlay_fn(Machine *m)
{
    AX = OVX_LIFTED;
    lift_ret_far(m, 0);
}

static LiftedEntry overlay_entries[1];

static void put16(uint8_t *at, unsigned v) { at[0] = (uint8_t)v; at[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *at, uint32_t v) { put16(at, v & 0xFFFF); put16(at + 2, v >> 16); }

/* OVL.EXE: opens itself, reads its one overlay into a fresh block, far-calls it, exits with AL. */
static size_t write_overlaid_exe(uint8_t *exe, size_t cap)
{
    size_t module_end = OVX_HDR + OVX_IMAGE, overlay_off = module_end + OVX_TRAILER;
    size_t len = overlay_off + OVX_DATA;
    if (len > cap)
        exit(1);
    memset(exe, 0, len);
    Prog p = {.org = 0};
    e8(&p, 0x0E); e8(&p, 0x1F);                                  /* push cs / pop ds */
    mov_imm(&p, R_DX, OVX_NAME); dos(&p, 0x3D00); ok(&p, 1);
    mov_rr(&p, R_BX, R_AX);
    mov_imm(&p, R_CX, (unsigned)(overlay_off >> 16)); mov_imm(&p, R_DX, overlay_off & 0xFFFF);
    dos(&p, 0x4200); ok(&p, 2);
    e8(&p, 0x53);                                                /* push bx */
    mov_imm(&p, R_BX, 0x10); dos(&p, 0x4800); ok(&p, 3);
    e8(&p, 0x5B);                                                /* pop bx */
    mov_sreg(&p, SR_DS, R_AX);
    mov_imm(&p, R_CX, sizeof OVX_CODE); mov_imm(&p, R_DX, 0); dos(&p, 0x3F00); ok(&p, 4);
    e8(&p, 0x0E);                                                /* push cs */
    e8(&p, 0xB8); uint16_t ret_imm = p.pc; e16(&p, 0);            /* mov ax, back */
    e8(&p, 0x50); e8(&p, 0x1E);                                  /* push ax / push ds */
    mov_imm(&p, R_AX, 0); e8(&p, 0x50); e8(&p, 0xCB);            /* push 0 / retf */
    put16(p.b + ret_imm, p.pc);
    e8(&p, 0xB4); e8(&p, 0x4C); int21(&p);                       /* back: exit with AL */
    memcpy(exe + OVX_HDR, p.b, p.pc);
    memcpy(exe + OVX_HDR + OVX_NAME, "OVL.EXE", 8);
    uint8_t *row = exe + OVX_HDR + OVX_TABLE;                    /* segment table: one stub */
    put16(row, OVX_STUB_SEG); put16(row + 2, 0x25); put16(row + 4, 3);
    uint8_t *stub = exe + OVX_HDR + OVX_STUB_SEG * 16;
    stub[0] = 0xCD; stub[1] = 0x3F;
    put32(stub + 4, 0); put16(stub + 8, sizeof OVX_CODE); put16(stub + 10, 0); put16(stub + 12, 1);
    memcpy(exe + module_end, "FBOV", 4);
    put32(exe + module_end + 4, OVX_DATA); put32(exe + module_end + 8, OVX_HDR + OVX_TABLE);
    put32(exe + module_end + 12, 1);
    memcpy(exe + overlay_off, OVX_CODE, sizeof OVX_CODE);
    const uint16_t hdr[] = {0x5A4D, module_end % 512, (module_end + 511) / 512, 0, OVX_HDR / 16, 0x20,
                            0x20, 0x50, 0x100, 0, 0, 0, 0x1C, 0};
    for (size_t i = 0; i < sizeof hdr / sizeof hdr[0]; i++)
        put16(exe + i * 2, hdr[i]);
    write_host("OVL.EXE", exe, len);
    return len;
}

static int run_overlaid(bool with_lift)
{
    Machine *m = new_machine();
    if (with_lift) {
        overlay_entries[0] = (LiftedEntry){ .module = "OVL.EXE#0030", .fn = lifted_overlay_fn,
                                            .name = "overlay_fn", .scratch_regs = LIFT_REG_FLAGS,
                                            .unknown_mask = 0xFFF & ~0xFu }; /* past the code */
        memcpy(overlay_entries[0].signature, OVX_CODE, sizeof OVX_CODE);
        lift_add_table(m, overlay_entries, 1);
    }
    CHECK(dos_start_program(m, "C:\\OVL.EXE", NULL));
    for (int i = 0; i < 100000 && !m->exit_requested; i++)
        machine_step(m); /* dispatches hooks, which cpu_run alone leaves pending */
    int code = m->exit_requested ? m->exit_code : -1;
    dos_shutdown(m);
    machine_destroy(m);
    return code;
}

static void test_overlays(void)
{
    make_root("overlay");
    static uint8_t exe[OVX_HDR + OVX_IMAGE + OVX_TRAILER + OVX_DATA];
    size_t len = write_overlaid_exe(exe, sizeof exe);
    BorlandOverlay list[4];
    CHECK(borland_overlays_parse(exe, len, list, 4) == 1);
    CHECK(list[0].stub_seg == OVX_STUB_SEG && list[0].file_off == OVX_HDR + OVX_IMAGE + OVX_TRAILER);
    CHECK(list[0].code_bytes == sizeof OVX_CODE && list[0].fixup_bytes == 0);
    CHECK(borland_overlays_parse(exe, OVX_HDR + OVX_IMAGE, list, 4) == 0); /* no trailer */
    CHECK(run_overlaid(false) == OVX_ORIGINAL);
    CHECK(run_overlaid(true) == OVX_LIFTED);
    remove_tree(root);
}

int main(void)
{
    test_allocator();
    test_psp_and_env();
    test_files();
    test_journal();
    test_memory_program();
    test_exec();
    test_overlays();
    if (failures)
        fprintf(stderr, "test_dos: %d failure(s)\n", failures);
    else
        fprintf(stderr, "test_dos: all passed\n");
    return failures ? 1 : 0;
}
