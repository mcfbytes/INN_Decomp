/* Mixed mode (docs/native.md section 11): a child that runs on the emulator while INN runs natively. The
 * child sees the native model's masks, video and keyboard bytes, reprograms the PIT, hooks vector 1Ch and
 * exits without restoring it; afterwards the native timer has the child's divisor, the stale vector is
 * restored and logged, and the machine's tickers are as they were. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "devices.h"
#include "dos_int.h"
#include "host_input.h"
#include "host_link.h"
#include "host_platform.h"
#include "host_timer.h"
#include "host_video.h"
#include "lift_internal.h"
#include "native.h"
#include "native/native_exec.h"

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

enum {
    COM_SIZE = 0x100, SEEN = 0x500, /* the child's notes, at 0000:0500 (below DOS's data) */
    SEEN_MASTER = SEEN, SEEN_SLAVE, SEEN_MODE, SEEN_PIXEL, SEEN_KEY,
    NATIVE_MASTER = 0xAA, NATIVE_SLAVE = 0x5B, CHILD_MASTER = 0xE3, CHILD_DIVISOR = 0x2000,
    NATIVE_DIVISOR = 0x4000, PIXEL = 0x5A, KEY_A = 0x1E, STALE_VECTOR = 0x1C, STALE_OFF = 0x0123,
    EXIT_CODE = 7, PUMPED_MIN = 10,
};

/* What the child does, in 8086 code: spin (0.2 s of emulated time), note what the devices held, set
 * its own mask, divisor, speaker gates and video mode, hook vector 1Ch with interrupts off, exit. */
static const uint8_t CHILD[] = {
    0xBA, 0x40, 0x00, 0xB9, 0x00, 0x00, 0xE2, 0xFE, 0x4A, 0x75, 0xF8,                 /* spin */
    0x31, 0xC0, 0x8E, 0xC0,                                                           /* es = 0 */
    0xE4, 0x60, 0x26, 0xA2, 0x04, 0x05,                                               /* port 60h */
    0xE4, 0x21, 0x26, 0xA2, 0x00, 0x05,                                               /* master mask */
    0xE4, 0xA1, 0x26, 0xA2, 0x01, 0x05,                                               /* slave mask */
    0xB4, 0x0F, 0xCD, 0x10, 0x26, 0xA2, 0x02, 0x05,                                   /* video mode */
    0xB8, 0x00, 0xA0, 0x8E, 0xD8, 0xA0, 0x00, 0x00, 0x26, 0xA2, 0x03, 0x05,           /* pixel 0 */
    0x0E, 0x1F,                                                                       /* ds = cs */
    0xB0, CHILD_MASTER, 0xE6, 0x21,                                                   /* out 21h */
    0xB0, 0x36, 0xE6, 0x43, 0xB8, 0x00, 0x20, 0xE6, 0x40, 0x88, 0xE0, 0xE6, 0x40,     /* PIT 0 */
    0xB0, 0x03, 0xE6, 0x61,                                                           /* speaker gates */
    0xB8, 0x03, 0x00, 0xCD, 0x10,                                                     /* mode 3 */
    0xFA, 0x26, 0xC7, 0x06, STALE_VECTOR * 4, 0x00, 0x23, 0x01,                       /* cli; vector 1Ch */
    0x8C, 0xC8, 0x26, 0xA3, STALE_VECTOR * 4 + 2, 0x00,
    0xB8, EXIT_CODE, 0x4C, 0xCD, 0x21,                                                /* exit */
};

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
        char path[1300];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

static void write_com(const char *name, const uint8_t *code, size_t len)
{
    uint8_t file[COM_SIZE];
    memset(file, 0x90, sizeof file);
    memcpy(file, code, len);
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", root, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fwrite(file, 1, sizeof file, f);
    fclose(f);
}

static void make_root(void)
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
    snprintf(root, sizeof root, "%s/test_mixed_%d", base, (int)getpid());
    remove_tree(root);
    if (mkdir(root, 0777) != 0) {
        perror(root);
        exit(1);
    }
}

/* ---- the native parent: its start is a lifted entry that runs the child through native_exec ---- */

static int child_code;

static void lifted_parent(Machine *m)
{
    static const uint8_t tail[2] = { 0, '\r' };
    uint16_t err = 0;
    ES = DS;
    BX = 0x100;
    AX = 0x4A00;
    guest_int(m, 0x21); /* give memory back, as TSNEXEC does before it starts programs */
    child_code = native_exec(m, "C:\\CHILD.COM", tail, 0, &err);
    AX = 0x4C00;
    guest_int(m, 0x21);
}

static LiftedEntry table[1];

/* mov ax, 4C00h / int 21h, padded with NOPs: the file the lifted entry stands for. */
static void parent_file(uint8_t *file)
{
    static const uint8_t EXIT[] = { 0xB8, 0x00, 0x4C, 0xCD, 0x21 };
    memset(file, 0x90, COM_SIZE);
    memcpy(file, EXIT, sizeof EXIT);
}

static void register_parent(Machine *m)
{
    table[0] = (LiftedEntry){ .module = "PARENT.COM", .seg = 0, .off = 0, .fn = lifted_parent, .name = "parent",
                              .scratch_regs = LIFT_REG_AX, .native_only = false };
    uint8_t file[COM_SIZE];
    parent_file(file);
    memcpy(table[0].signature, file, LIFT_SIGNATURE_LEN);
    lift_add_table(m, table, 1);
}

/* ---- link pump: counts the runs the ticker makes ---- */

typedef struct Pipe { int unused; } Pipe;
static int pumped;
static void count_pump(Machine *m) { (void)m; pumped++; }
static int pipe_send(void *c, const uint8_t *b, int n) { (void)c, (void)b; return n; }
static int pipe_recv(void *c, uint8_t *b, int n) { (void)c, (void)b, (void)n; return 0; }
static bool pipe_up(void *c) { (void)c; return true; }
static void pipe_close(void *c) { (void)c; }

static bool log_has(const char *path, const char *needle)
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

static void test_game_round_trip(void)
{
    make_root();
    write_com("CHILD.COM", CHILD, sizeof CHILD);
    uint8_t parent[COM_SIZE];
    parent_file(parent);
    write_com("PARENT.COM", parent, sizeof parent);
    Machine *m = machine_create();
    bios_init(m);
    register_parent(m);
    dos_init(m, root);
    NativeConfig cfg = { .deterministic = true, .strict = true, .call_cost_ns = 1000, .yield_every_us = 4000 };
    native_init(m, &cfg);
    native_set_platform(m, NULL);
    native_set_active(m, true);

    /* What the native model holds when the game starts. */
    native_pic_set_mask(m, 0, NATIVE_MASTER);
    native_pic_set_mask(m, 1, NATIVE_SLAVE);
    host_timer_set_divisor(m, NATIVE_DIVISOR);
    native_set_port61(m, 0x30);
#ifdef INN_NATIVE
    host_video_set_mode(m, HOST_VIDEO_320X200X8, true);
    mem_wr8(m, 0xA0000, PIXEL);
    mem_wr8(m, 0x449, HOST_VIDEO_320X200X8); /* the BIOS data area's mode byte, which INT 10h reads */
#endif
    host_input_key(m, KEY_A, true); /* queued, not yet delivered */
    SerialLink link = { .ctx = NULL, .send = pipe_send, .recv = pipe_recv, .connected = pipe_up,
                        .close = pipe_close };
    CHECK(host_link_attach(m, link) == 0);
    host_link_set_pump(m, count_pump);
    int tickers = m->ticker_count;
    pumped = 0;
    uint32_t vector_before = mem_rd32(m, STALE_VECTOR * 4);
    uint16_t base_psp = dos_state(m)->cur_psp;

    char logpath[1200];
    snprintf(logpath, sizeof logpath, "%s/log.txt", root);
    fflush(stderr);
    int saved_stderr = dup(2);
    CHECK(freopen(logpath, "w", stderr) != NULL);
    static const uint8_t tail[2] = { 0, '\r' };
    uint16_t err = 0;
    int code = native_exec(m, "C:\\PARENT.COM", tail, 0, &err);
    fflush(stderr);
    dup2(saved_stderr, 2);
    close(saved_stderr);
    clearerr(stderr);

    CHECK(code == 0 && child_code == EXIT_CODE);
    CHECK(dos_state(m)->cur_psp == base_psp);
    CHECK(m->native);
    /* In the child: the native model's masks, key byte and picture. */
    CHECK(mem_rd8(m, SEEN_MASTER) == NATIVE_MASTER);
    CHECK(mem_rd8(m, SEEN_SLAVE) == NATIVE_SLAVE);
    CHECK(mem_rd8(m, SEEN_KEY) == KEY_A);
#ifdef INN_NATIVE
    CHECK(mem_rd8(m, SEEN_MODE) == HOST_VIDEO_320X200X8);
    CHECK(mem_rd8(m, SEEN_PIXEL) == PIXEL);
    CHECK(m->native_video && host_video_mode(m) == HOST_VIDEO_TEXT80); /* the child's mode came back */
#endif
    /* Back: what the child programmed. */
    CHECK(host_timer_divisor(m) == CHILD_DIVISOR);
    CHECK(native_pic_mask(m, 0) == CHILD_MASTER && native_pic_mask(m, 1) == NATIVE_SLAVE);
    CHECK((native_port61(m) & 3) == 3);
    CHECK(mem_rd32(m, STALE_VECTOR * 4) == vector_before);
    CHECK(log_has(logpath, "native: vector 1C left at"));
    CHECK(m->ticker_count == tickers);
    int pumped_in_child = pumped;
    CHECK(pumped_in_child >= PUMPED_MIN);
    m->virtual_us += 5000;
    for (int i = 0; i < m->ticker_count; i++)
        m->tickers[i](m);
    CHECK(pumped == pumped_in_child); /* the ticker is gone; only safe points pump now */
    CHECK(native_pic_mask(m, 0) == CHILD_MASTER);

    native_set_active(m, false);
    dos_shutdown(m);
    bios_shutdown(m);
    machine_destroy(m);
    remove_tree(root);
}

int main(void)
{
    test_game_round_trip();
    if (failures)
        fprintf(stderr, "test_mixed: %d failure(s)\n", failures);
    else
        fprintf(stderr, "test_mixed: all passed\n");
    return failures ? 1 : 0;
}
