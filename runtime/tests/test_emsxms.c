/* EMS and XMS (emsxms.c): off by default; allocate, map, move across conventional, EMS and XMS memory,
 * free, error codes, the EMMXXXX0 device, and the state living inside the snapshotted memory. Guest
 * code is a few bytes (INT 67h / INT 2Fh / INT 21h / CALL FAR to the XMS entry, then HLT). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dos.h"
#include "emsxms.h"
#include "machine.h"

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

enum { CODE_SEG = 0x0700, DATA_SEG = 0x2000, STACK_SEG = 0x3000, BUF_A = 0x0100, BUF_B = 0x0400, BLOCK = 0x0800 };

static char root[1024];

static Machine *new_machine(unsigned ems_kb, unsigned xms_kb)
{
    Machine *m = machine_create();
    dos_init(m, root);
    CHECK(emsxms_init(m, ems_kb, xms_kb));
    return m;
}

static void finish(Machine *m)
{
    dos_shutdown(m);
    machine_destroy(m);
}

/* Runs code at CODE_SEG:0 (it ends in HLT) with DS = ES = DATA_SEG unless set already by regs. */
static void run(Machine *m, const uint8_t *code, size_t n)
{
    memcpy(m->mem + real_addr(CODE_SEG, 0), code, n);
    cpu_set_seg(&m->cpu, S_CS, CODE_SEG);
    cpu_set_seg(&m->cpu, S_SS, STACK_SEG);
    m->cpu.eip = 0;
    SP = 0xFFFE;
    m->cpu.halted = false;
    for (int i = 0; i < 100 && !m->cpu.halted; i++)
        cpu_run(m, 10000);
    CHECK(m->cpu.halted);
}

static void segs(Machine *m)
{
    cpu_set_seg(&m->cpu, S_DS, DATA_SEG);
    cpu_set_seg(&m->cpu, S_ES, DATA_SEG);
}

static void int_n(Machine *m, uint8_t vector)
{
    const uint8_t code[] = { 0xCD, vector, 0xF4 };
    run(m, code, sizeof code);
}

static uint8_t ems(Machine *m, uint16_t ax, uint16_t bx, uint16_t dx)
{
    segs(m);
    AX = ax, BX = bx, DX = dx;
    int_n(m, 0x67);
    return AH;
}

static void xms(Machine *m, uint16_t ax, uint16_t dx)
{
    uint32_t entry = xms_entry(m);
    const uint8_t code[] = { 0x9A, (uint8_t)entry, (uint8_t)(entry >> 8), (uint8_t)(entry >> 16), (uint8_t)(entry >> 24), 0xF4 };
    segs(m);
    AX = ax, DX = dx, BX = 0xFFFF;
    run(m, code, sizeof code);
}

static uint8_t *data(Machine *m, uint16_t off) { return m->mem + real_addr(DATA_SEG, off); }
static uint8_t *frame(Machine *m, int slot, uint16_t off) { return m->mem + real_addr(EMS_FRAME_SEG, (uint16_t)(slot * EMS_PAGE_BYTES + off)); }

static void fill(uint8_t *p, size_t n, uint8_t seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed + i * 7);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v), put16(p + 2, (uint16_t)(v >> 16)); }

/* EMS 57h block at DATA_SEG:BLOCK: type 0 = conventional (seg:off), 1 = EMS (handle, page:off). */
static uint8_t ems_move(Machine *m, uint32_t len, uint8_t st, uint16_t sh, uint16_t soff, uint16_t sseg,
                        uint8_t dt, uint16_t dh, uint16_t doff, uint16_t dseg, bool exchange)
{
    uint8_t *b = data(m, BLOCK);
    put32(b, len);
    b[4] = st, put16(b + 5, sh), put16(b + 7, soff), put16(b + 9, sseg);
    b[11] = dt, put16(b + 12, dh), put16(b + 14, doff), put16(b + 16, dseg);
    segs(m);
    SI = BLOCK;
    AX = exchange ? 0x5701 : 0x5700;
    int_n(m, 0x67);
    return AH;
}

/* XMS 0Bh block at DATA_SEG:BLOCK; handle 0 takes a seg:off as the offset. */
static void xms_move(Machine *m, uint32_t len, uint16_t sh, uint32_t soff, uint16_t dh, uint32_t doff)
{
    uint8_t *b = data(m, BLOCK);
    put32(b, len), put16(b + 4, sh), put32(b + 6, soff), put16(b + 10, dh), put32(b + 12, doff);
    uint32_t entry = xms_entry(m);
    const uint8_t code[] = { 0x9A, (uint8_t)entry, (uint8_t)(entry >> 8), (uint8_t)(entry >> 16), (uint8_t)(entry >> 24), 0xF4 };
    segs(m);
    SI = BLOCK;
    AX = 0x0B00;
    BX = 0xFFFF;
    run(m, code, sizeof code);
}

static uint32_t far(uint16_t seg, uint16_t off) { return (uint32_t)seg << 16 | off; }

/* ---- off by default ---- */

static void test_off(void)
{
    Machine *m = new_machine(0, 0);
    CHECK(m->ems == NULL && m->xms == NULL);
    CHECK(m->snapshot_bytes == MEM_ONE_MEG + 0x10000);
    CHECK(ems(m, 0x4000, 0, 0) == 0x84);
    segs(m);
    AX = 0x4300;
    int_n(m, 0x2F);
    CHECK(AL == 0);
    memcpy(data(m, BUF_A), "EMMXXXX0", 9);
    segs(m);
    AX = 0x3D00, DX = BUF_A;
    int_n(m, 0x21);
    CHECK(AX == 2); /* file not found */
    m->mem[real_addr(EMS_FRAME_SEG, 0)] = 0xFF;
    mem_wr8(m, real_addr(EMS_FRAME_SEG, 0), 0x12); /* no frame: read-only as before */
    CHECK(m->mem[real_addr(EMS_FRAME_SEG, 0)] == 0xFF);
    finish(m);
}

/* ---- EMS ---- */

static void test_ems_device(Machine *m)
{
    memcpy(data(m, BUF_A), "EMMXXXX0", 9);
    segs(m);
    AX = 0x3D00, DX = BUF_A;
    int_n(m, 0x21);
    uint16_t handle = AX;
    CHECK(handle >= 5);
    BX = handle, AX = 0x4400;
    int_n(m, 0x21);
    CHECK(DX & 0x80);
    BX = handle, AX = 0x4407;
    int_n(m, 0x21);
    CHECK(AL == 0xFF);
    BX = handle, AX = 0x3E00;
    int_n(m, 0x21);
}

static void test_ems(void)
{
    Machine *m = new_machine(1024, 0);
    CHECK(m->ems && !m->xms);
    CHECK(m->snapshot_bytes > EXT_MEM_BASE + 1024 * 1024);
    test_ems_device(m);
    CHECK(ems(m, 0x4000, 0, 0) == 0);
    CHECK(ems(m, 0x4600, 0, 0) == 0 && AL == 0x40);
    CHECK(ems(m, 0x4100, 0, 0) == 0 && BX == EMS_FRAME_SEG);
    CHECK(ems(m, 0x4200, 0, 0) == 0 && BX == 64 && DX == 64);
    CHECK(ems(m, 0x4300, 4, 0) == 0);
    uint16_t h = DX;
    CHECK(h != 0);
    CHECK(ems(m, 0x4200, 0, 0) == 0 && BX == 60);
    CHECK(ems(m, 0x4300, 0, 0) == 0x89);
    CHECK(ems(m, 0x4300, 65, 0) == 0x87);
    CHECK(ems(m, 0x4300, 61, 0) == 0x88);

    /* map, write, remap: pages keep their contents */
    CHECK(ems(m, 0x4400, 0, h) == 0);
    fill(frame(m, 0, 0), EMS_PAGE_BYTES, 1);
    mem_wr8(m, real_addr(EMS_FRAME_SEG, 5), 0xA5); /* the frame is writable through the CPU path */
    CHECK(*frame(m, 0, 5) == 0xA5);
    CHECK(ems(m, 0x4400, 1, h) == 0);
    CHECK(*frame(m, 0, 0) == 0 && *frame(m, 0, 100) == 0);
    fill(frame(m, 0, 0), EMS_PAGE_BYTES, 2);
    CHECK(ems(m, 0x4401, 0, h) == 0); /* logical 0 into slot 1 */
    CHECK(*frame(m, 1, 5) == 0xA5 && *frame(m, 1, 6) == (uint8_t)(1 + 6 * 7));
    CHECK(ems(m, 0x4400, 0, h) == 0);
    CHECK(*frame(m, 0, 5) == 0xA5);

    /* save and restore the map */
    CHECK(ems(m, 0x4800, 0, h) == 0x8E);
    CHECK(ems(m, 0x4700, 0, h) == 0);
    CHECK(ems(m, 0x4700, 0, h) == 0x8D);
    CHECK(ems(m, 0x4400, 1, h) == 0);
    CHECK(*frame(m, 0, 0) == 2);
    CHECK(ems(m, 0x4500, 0, h) == 0x86); /* a saved context is outstanding */
    CHECK(ems(m, 0x4800, 0, h) == 0);
    CHECK(*frame(m, 0, 5) == 0xA5);

    /* errors */
    CHECK(ems(m, 0x4400, 4, h) == 0x8A);
    CHECK(ems(m, 0x4404, 0, h) == 0x8B);
    CHECK(ems(m, 0x4400, 0, 77) == 0x83);
    CHECK(ems(m, 0x6000, 0, 0) == 0x84);

    /* counts */
    CHECK(ems(m, 0x4B00, 0, 0) == 0 && BX == 2); /* the system handle and h */
    CHECK(ems(m, 0x4C00, 0, h) == 0 && BX == 4);
    segs(m);
    DI = BUF_B;
    AX = 0x4D00;
    int_n(m, 0x67);
    CHECK(AH == 0 && BX == 2 && data(m, BUF_B)[4] == h && data(m, BUF_B)[6] == 4);

    /* 57h: conventional to EMS across a page boundary and back, then into a mapped page */
    fill(data(m, BUF_A), 64, 9);
    CHECK(ems_move(m, 64, 0, 0, BUF_A, DATA_SEG, 1, h, 0x3FE0, 2, false) == 0);
    memset(data(m, BUF_B), 0, 64);
    CHECK(ems_move(m, 64, 1, h, 0x3FE0, 2, 0, 0, BUF_B, DATA_SEG, false) == 0);
    CHECK(memcmp(data(m, BUF_A), data(m, BUF_B), 64) == 0);
    CHECK(ems(m, 0x4402, 3, h) == 0); /* logical 3 into slot 2: the move's tail shows there */
    CHECK(memcmp(frame(m, 2, 0), data(m, BUF_A) + 32, 32) == 0);
    fill(data(m, BUF_A), 16, 50);
    CHECK(ems_move(m, 16, 0, 0, BUF_A, DATA_SEG, 1, h, 0x10, 3, false) == 0);
    CHECK(memcmp(frame(m, 2, 0x10), data(m, BUF_A), 16) == 0);
    /* a conventional address inside a mapped slot reads the page */
    CHECK(ems_move(m, 16, 0, 0, (uint16_t)(2 * EMS_PAGE_BYTES + 0x10), EMS_FRAME_SEG, 0, 0, BUF_B, DATA_SEG, false) == 0);
    CHECK(memcmp(data(m, BUF_B), data(m, BUF_A), 16) == 0);
    /* exchange */
    memset(data(m, BUF_B), 0x33, 16);
    CHECK(ems_move(m, 16, 0, 0, BUF_B, DATA_SEG, 1, h, 0x10, 3, true) == 0);
    CHECK(data(m, BUF_B)[0] == (uint8_t)50 && *frame(m, 2, 0x10) == 0x33);
    CHECK(ems_move(m, 16, 1, h, 0x4000, 0, 0, 0, BUF_B, DATA_SEG, false) == 0x95);
    CHECK(ems_move(m, 16, 1, h, 0, 4, 0, 0, BUF_B, DATA_SEG, false) == 0x8A);
    CHECK(ems_move(m, 32, 1, h, 0x3FF0, 3, 0, 0, BUF_B, DATA_SEG, false) == 0x93);
    CHECK(ems_move(m, 0x100001, 1, h, 0, 0, 0, 0, BUF_B, DATA_SEG, false) == 0x96);

    /* free */
    CHECK(ems(m, 0x4500, 0, h) == 0);
    CHECK(ems(m, 0x4200, 0, 0) == 0 && BX == 64);
    CHECK(ems(m, 0x4500, 0, h) == 0x83);
    CHECK(ems(m, 0x4C00, 0, h) == 0x83);
    finish(m);
}

/* ---- XMS ---- */

static void test_xms(void)
{
    Machine *m = new_machine(0, 1024);
    CHECK(!m->ems && m->xms);
    segs(m);
    AX = 0x4300;
    int_n(m, 0x2F);
    CHECK(AL == 0x80);
    AX = 0x4310;
    int_n(m, 0x2F);
    CHECK(far(m->cpu.seg[S_ES], BX) == xms_entry(m));
    CHECK(ems(m, 0x4000, 0, 0) == 0x84); /* no EMS */

    xms(m, 0x0000, 0);
    CHECK(AX == 0x0300);
    xms(m, 0x0800, 0);
    CHECK(AX == 1024 && DX == 1024 && BL == 0);
    xms(m, 0x0900, 100);
    CHECK(AX == 1);
    uint16_t h1 = DX;
    xms(m, 0x0900, 2000);
    CHECK(AX == 0 && BL == 0xA0);
    xms(m, 0x0900, 200);
    CHECK(AX == 1);
    uint16_t h2 = DX;
    xms(m, 0x0800, 0);
    CHECK(AX == 724 && DX == 724);

    /* moves: conventional -> h1 -> h2 -> conventional */
    fill(data(m, BUF_A), 256, 3);
    xms_move(m, 256, 0, far(DATA_SEG, BUF_A), h1, 100 * 1024 - 256);
    CHECK(AX == 1);
    xms_move(m, 256, h1, 100 * 1024 - 256, h2, 5);
    CHECK(AX == 1);
    memset(data(m, BUF_B), 0, 256);
    xms_move(m, 256, h2, 5, 0, far(DATA_SEG, BUF_B));
    CHECK(AX == 1 && memcmp(data(m, BUF_A), data(m, BUF_B), 256) == 0);
    xms_move(m, 257, 0, far(DATA_SEG, BUF_A), h1, 100 * 1024 - 256);
    CHECK(AX == 0 && BL == 0xA6);
    xms_move(m, 16, 99, 0, h1, 0);
    CHECK(AX == 0 && BL == 0xA3);
    xms_move(m, 16, h1, 0, 99, 0);
    CHECK(AX == 0 && BL == 0xA5);
    xms_move(m, 16, h1, 100 * 1024, 0, far(DATA_SEG, BUF_B));
    CHECK(AX == 0 && BL == 0xA4);

    /* lock: the linear address holds what was moved there */
    xms(m, 0x0C00, h2);
    CHECK(AX == 1);
    uint32_t linear = (uint32_t)DX << 16 | BX;
    CHECK(linear >= EXT_MEM_BASE && linear + 200 * 1024 <= m->snapshot_bytes);
    CHECK(memcmp(m->mem + linear + 5, data(m, BUF_A), 256) == 0);
    xms(m, 0x0E00, h2);
    CHECK(AX == 1 && BH == 1 && DX == 200);
    xms(m, 0x0A00, h2);
    CHECK(AX == 0 && BL == 0xAB);
    xms(m, 0x0D00, h2);
    CHECK(AX == 1);
    xms(m, 0x0D00, h2);
    CHECK(AX == 0 && BL == 0xAA);

    /* free, reuse of the gap, errors */
    xms(m, 0x0A00, h1);
    CHECK(AX == 1);
    xms(m, 0x0A00, h1);
    CHECK(AX == 0 && BL == 0xA2);
    xms(m, 0x0800, 0);
    CHECK(AX == 724 && DX == 824);
    xms(m, 0x0900, 50);
    CHECK(AX == 1);
    uint16_t h3 = DX;
    xms(m, 0x0C00, h3);
    CHECK(((uint32_t)DX << 16 | BX) < linear); /* first fit: the freed gap below h2 */
    xms(m, 0x0D00, h3);
    xms(m, 0x0100, 0);
    CHECK(AX == 0 && BL == 0x90);
    xms(m, 0x2000, 0);
    CHECK(AX == 0 && BL == 0x80);
    finish(m);
}

/* ---- both, and snapshots ---- */

static void test_both(void)
{
    Machine *m = new_machine(256, 256);
    CHECK(m->ems && m->xms);
    CHECK(ems(m, 0x4200, 0, 0) == 0 && BX == 16);
    CHECK(ems(m, 0x4300, 2, 0) == 0);
    uint16_t eh = DX;
    CHECK(ems(m, 0x4401, 1, eh) == 0);
    xms(m, 0x0900, 64);
    uint16_t xh = DX;
    /* XMS -> a conventional address in a mapped EMS slot lands in the EMS page */
    fill(data(m, BUF_A), 128, 11);
    xms_move(m, 128, 0, far(DATA_SEG, BUF_A), xh, 0);
    xms_move(m, 128, xh, 0, 0, far(EMS_FRAME_SEG, EMS_PAGE_BYTES + 0x20));
    CHECK(AX == 1 && memcmp(frame(m, 1, 0x20), data(m, BUF_A), 128) == 0);
    CHECK(ems(m, 0x4401, 0, eh) == 0); /* map another page over the slot, then read the first by 57h */
    memset(data(m, BUF_B), 0, 128);
    CHECK(ems_move(m, 128, 1, eh, 0x20, 1, 0, 0, BUF_B, DATA_SEG, false) == 0);
    CHECK(memcmp(data(m, BUF_B), data(m, BUF_A), 128) == 0);

    /* every bit of state is inside [0, snapshot_bytes): restoring a copy undoes later calls */
    uint32_t bytes = m->snapshot_bytes;
    uint8_t *snap = malloc(bytes);
    CHECK(snap != NULL);
    memcpy(snap, m->mem, bytes);
    CHECK(ems(m, 0x4300, 10, 0) == 0);
    CHECK(ems(m, 0x4400, 0, DX) == 0);
    xms(m, 0x0900, 100);
    xms(m, 0x0A00, xh);
    memcpy(m->mem, snap, bytes);
    CHECK(ems(m, 0x4200, 0, 0) == 0 && BX == 14);
    CHECK(ems(m, 0x4B00, 0, 0) == 0 && BX == 2);
    xms(m, 0x0800, 0);
    CHECK(AX == 192 && DX == 192);
    xms(m, 0x0E00, xh);
    CHECK(AX == 1 && DX == 64);
    free(snap);
    finish(m);
}

static void test_limits(void)
{
    Machine *m = machine_create();
    CHECK(!emsxms_init(m, 16384, 0));      /* 1024 pages and the rest do not fit under 16 MB */
    CHECK(!emsxms_init(m, 8192, 8192));
    CHECK(m->ems == NULL && m->xms == NULL && m->snapshot_bytes == MEM_ONE_MEG + 0x10000);
    CHECK(emsxms_init(m, 4096, 8192));
    CHECK(!emsxms_init(m, 16, 0));         /* once only */
    machine_destroy(m);
}

int main(void)
{
    snprintf(root, sizeof root, "%s", "test_emsxms_drive");
    mkdir(root, 0777);
    test_off();
    test_ems();
    test_xms();
    test_both();
    test_limits();
    rmdir(root);
    if (failures) {
        fprintf(stderr, "test_emsxms: %d failures\n", failures);
        return 1;
    }
    printf("test_emsxms: ok\n");
    return 0;
}
