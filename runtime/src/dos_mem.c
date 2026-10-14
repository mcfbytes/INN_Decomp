/* The DOS memory arena: a chain of memory control blocks in guest RAM, exactly as programs that
 * walk it (via SYSVARS) expect, with first/best/last-fit allocation. */
#define _POSIX_C_SOURCE 200809L /* fds, directories, localtime_r on any libc */
#include <string.h>

#include "dos_int.h"

enum { MCB_TYPE = 0, MCB_OWNER = 1, MCB_SIZE = 3, MCB_NAME = 8, MCB_NAME_LEN = 8 };
enum { MCB_MID = 'M', MCB_LAST = 'Z', OWNER_FREE = 0 };
enum { FIT_FIRST = 0, FIT_BEST = 1, FIT_LAST = 2, FIT_MASK = 3 };

static uint8_t mcb_type(Machine *m, uint16_t mcb) { return mem_rd8(m, real_addr(mcb, MCB_TYPE)); }
static uint16_t mcb_owner(Machine *m, uint16_t mcb) { return mem_rd16(m, real_addr(mcb, MCB_OWNER)); }
static uint16_t mcb_size(Machine *m, uint16_t mcb) { return mem_rd16(m, real_addr(mcb, MCB_SIZE)); }
static bool mcb_valid(Machine *m, uint16_t mcb)
{
    uint8_t t = mcb_type(m, mcb);
    return mcb < DOS_ARENA_END && (t == MCB_MID || t == MCB_LAST);
}

static void mcb_write(Machine *m, uint16_t mcb, uint8_t type, uint16_t owner, uint16_t size)
{
    mem_wr8(m, real_addr(mcb, MCB_TYPE), type);
    mem_wr16(m, real_addr(mcb, MCB_OWNER), owner);
    mem_wr16(m, real_addr(mcb, MCB_SIZE), size);
}

static uint16_t first_mcb(Machine *m) { return mem_rd16(m, real_addr(DOS_DATA_SEG, DD_FIRST_MCB)); }

void dos_mem_init(Machine *m, uint16_t first, uint16_t end)
{
    mcb_write(m, first, MCB_LAST, OWNER_FREE, (uint16_t)(end - first - 1));
    for (int i = 0; i < MCB_NAME_LEN; i++)
        mem_wr8(m, real_addr(first, (uint16_t)(MCB_NAME + i)), 0);
    mem_wr16(m, real_addr(DOS_DATA_SEG, DD_FIRST_MCB), first);
}

/* Merges each free block with the free blocks that follow it. */
static int coalesce(Machine *m)
{
    uint16_t mcb = first_mcb(m);
    for (;;) {
        if (!mcb_valid(m, mcb))
            return DE_MCB_DESTROYED;
        if (mcb_type(m, mcb) == MCB_LAST)
            return DE_OK;
        uint16_t next = (uint16_t)(mcb + mcb_size(m, mcb) + 1);
        if (!mcb_valid(m, next))
            return DE_MCB_DESTROYED;
        if (mcb_owner(m, mcb) == OWNER_FREE && mcb_owner(m, next) == OWNER_FREE) {
            mcb_write(m, mcb, mcb_type(m, next), OWNER_FREE,
                      (uint16_t)(mcb_size(m, mcb) + mcb_size(m, next) + 1));
            continue;
        }
        mcb = next;
    }
}

/* Cuts block mcb to paras, turning the tail into a free block. */
static void split(Machine *m, uint16_t mcb, uint16_t paras)
{
    uint16_t size = mcb_size(m, mcb);
    if (size <= paras)
        return;
    uint8_t type = mcb_type(m, mcb);
    uint16_t tail = (uint16_t)(mcb + paras + 1);
    mcb_write(m, tail, type, OWNER_FREE, (uint16_t)(size - paras - 1));
    mcb_write(m, mcb, MCB_MID, mcb_owner(m, mcb), paras);
}

static bool better_fit(uint8_t strategy, uint16_t found_size, uint16_t best_size, bool have)
{
    switch (strategy & FIT_MASK) {
    case FIT_BEST: return !have || found_size < best_size;
    case FIT_LAST: return true;
    default: return !have;
    }
}

int dos_mem_alloc(Machine *m, uint16_t paras, uint16_t owner, uint16_t *seg, uint16_t *largest)
{
    int err = coalesce(m);
    if (err)
        return err;
    uint8_t strategy = dos_state(m)->alloc_strategy;
    uint16_t best = 0, best_size = 0, max_free = 0;
    bool have = false;
    for (uint16_t mcb = first_mcb(m);; mcb = (uint16_t)(mcb + mcb_size(m, mcb) + 1)) {
        uint16_t size = mcb_size(m, mcb);
        if (mcb_owner(m, mcb) == OWNER_FREE) {
            if (size > max_free)
                max_free = size;
            if (size >= paras && better_fit(strategy, size, best_size, have)) {
                best = mcb, best_size = size, have = true;
            }
        }
        if (mcb_type(m, mcb) == MCB_LAST)
            break;
    }
    *largest = max_free;
    if (!have)
        return DE_NO_MEMORY;
    if ((strategy & FIT_MASK) == FIT_LAST && best_size > paras) {
        uint8_t type = mcb_type(m, best);
        mcb_write(m, best, MCB_MID, OWNER_FREE, (uint16_t)(best_size - paras - 1));
        best = (uint16_t)(best + best_size - paras);
        mcb_write(m, best, type, OWNER_FREE, paras);
    } else {
        split(m, best, paras);
    }
    dos_mem_set_owner(m, (uint16_t)(best + 1), owner, "");
    *seg = (uint16_t)(best + 1);
    return DE_OK;
}

int dos_mem_free(Machine *m, uint16_t seg)
{
    uint16_t mcb = (uint16_t)(seg - 1);
    if (!mcb_valid(m, mcb))
        return DE_INVALID_BLOCK;
    mem_wr16(m, real_addr(mcb, MCB_OWNER), OWNER_FREE);
    return DE_OK;
}

/* Like MS-DOS, a failed grow still enlarges the block as far as it can go. */
int dos_mem_resize(Machine *m, uint16_t seg, uint16_t paras, uint16_t *largest)
{
    uint16_t mcb = (uint16_t)(seg - 1);
    if (!mcb_valid(m, mcb))
        return DE_INVALID_BLOCK;
    int err = coalesce(m);
    if (err)
        return err;
    uint16_t size = mcb_size(m, mcb);
    if (paras > size && mcb_type(m, mcb) == MCB_MID) {
        uint16_t next = (uint16_t)(mcb + size + 1);
        if (mcb_owner(m, next) == OWNER_FREE) {
            size = (uint16_t)(size + mcb_size(m, next) + 1);
            mcb_write(m, mcb, mcb_type(m, next), mcb_owner(m, mcb), size);
        }
    }
    if (paras > size) {
        *largest = size;
        return DE_NO_MEMORY;
    }
    split(m, mcb, paras);
    return coalesce(m);
}

void dos_mem_free_owner(Machine *m, uint16_t owner)
{
    for (uint16_t mcb = first_mcb(m); mcb_valid(m, mcb); mcb = (uint16_t)(mcb + mcb_size(m, mcb) + 1)) {
        if (mcb_owner(m, mcb) == owner)
            mem_wr16(m, real_addr(mcb, MCB_OWNER), OWNER_FREE);
        if (mcb_type(m, mcb) == MCB_LAST)
            break;
    }
}

void dos_mem_set_owner(Machine *m, uint16_t seg, uint16_t owner, const char *name)
{
    uint16_t mcb = (uint16_t)(seg - 1);
    mem_wr16(m, real_addr(mcb, MCB_OWNER), owner);
    size_t len = strlen(name);
    for (size_t i = 0; i < MCB_NAME_LEN; i++)
        mem_wr8(m, real_addr(mcb, (uint16_t)(MCB_NAME + i)), i < len ? (uint8_t)name[i] : 0);
}

uint16_t dos_mem_block_size(Machine *m, uint16_t seg) { return mcb_size(m, (uint16_t)(seg - 1)); }

/* ---- INT 21h handlers ---- */

CallbackResult dos_alloc(Machine *m)
{
    uint16_t seg, largest;
    int err = dos_mem_alloc(m, BX, dos_state(m)->cur_psp, &seg, &largest);
    if (err) {
        BX = largest;
        return dos_fail(m, (uint16_t)err);
    }
    AX = seg;
    return dos_ok(m);
}

CallbackResult dos_free(Machine *m)
{
    int err = dos_mem_free(m, ES);
    return err ? dos_fail(m, (uint16_t)err) : dos_ok(m);
}

CallbackResult dos_resize(Machine *m)
{
    uint16_t largest;
    int err = dos_mem_resize(m, ES, BX, &largest);
    if (err) {
        if (err == DE_NO_MEMORY)
            BX = largest;
        return dos_fail(m, (uint16_t)err);
    }
    return dos_ok(m);
}

enum { STRAT_GET = 0, STRAT_SET = 1, UMB_GET = 2, UMB_SET = 3 };

CallbackResult dos_strategy(Machine *m)
{
    switch (AL) {
    case STRAT_GET:
        AX = dos_state(m)->alloc_strategy;
        return dos_ok(m);
    case STRAT_SET:
        dos_state(m)->alloc_strategy = BL;
        return dos_ok(m);
    case UMB_GET:
        AL = 0; /* upper memory not linked */
        return dos_ok(m);
    case UMB_SET:
        return BX == 0 ? dos_ok(m) : dos_fail(m, DE_FUNCTION); /* there are no UMBs */
    }
    return dos_fail(m, DE_FUNCTION);
}
