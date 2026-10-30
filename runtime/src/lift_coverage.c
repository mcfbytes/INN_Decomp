/* Call coverage (docs/lifting.md, "Coverage"): distinct call and interrupt targets named relative to
 * the announced module that holds them, written when the machine is destroyed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lift.h"

enum { COV_MAX_NAMES = 256, COV_MAX_EXTENTS = 64 };

/* Where an announced module sits now. Programs cover their whole memory block, overlays and
 * drivers their image; the innermost extent names an address. */
typedef struct CovExtent {
    uint32_t lo, hi;
    uint16_t load_seg;
    int name;
    bool overlay;
} CovExtent;

typedef struct CovEntry {
    uint64_t key;  /* linear address, module generation above bit 20; 0 = empty slot */
    uint64_t calls;
    int name;      /* -1: outside every announced module */
    uint16_t seg, off;
    uint8_t via;   /* COV_VIA_* bits */
} CovEntry;

typedef struct Coverage {
    char *path;
    char names[COV_MAX_NAMES][32];
    int name_count;
    CovExtent extents[COV_MAX_EXTENTS];
    int extent_count;
    uint64_t generation; /* bumped whenever a module comes or goes, so cached names stay right */
    CovEntry *table;
    size_t capacity, used;
} Coverage;

static int intern(Coverage *c, const char *name)
{
    for (int i = 0; i < c->name_count; i++)
        if (!strcmp(c->names[i], name))
            return i;
    if (c->name_count == COV_MAX_NAMES)
        return -1;
    snprintf(c->names[c->name_count], sizeof c->names[0], "%s", name);
    return c->name_count++;
}

static void drop_range(Coverage *c, uint32_t lo, uint32_t hi)
{
    int kept = 0;
    for (int i = 0; i < c->extent_count; i++)
        if (c->extents[i].lo < lo || c->extents[i].hi > hi)
            c->extents[kept++] = c->extents[i];
    c->extent_count = kept;
    c->generation++;
}

void lift_coverage_enable(Machine *m, const char *path)
{
    Coverage *c = calloc(1, sizeof *c);
    if (!c || !(c->path = strdup(path)))
        fatal("out of memory");
    c->capacity = 4096;
    c->table = calloc(c->capacity, sizeof *c->table);
    if (!c->table)
        fatal("out of memory");
    m->coverage = c;
}

void lift_coverage_module(Machine *m, const char *module, uint16_t load_seg, uint32_t bytes, bool overlay)
{
    Coverage *c = m->coverage;
    if (!c)
        return;
    uint32_t lo = real_addr(load_seg, 0), hi = lo + bytes;
    drop_range(c, lo, hi);
    for (int i = 0; overlay && i < c->extent_count;) { /* code read over part of another overlay evicts it */
        const CovExtent *e = &c->extents[i];
        if (e->overlay && e->lo < hi && e->hi > lo)
            c->extents[i] = c->extents[--c->extent_count];
        else
            i++;
    }
    for (int i = 0; i < c->extent_count; i++) /* an overlay's previous home, a module's previous load */
        if (!strcmp(c->names[c->extents[i].name], module))
            c->extents[i] = c->extents[--c->extent_count];
    int name = intern(c, module);
    if (name < 0 || c->extent_count == COV_MAX_EXTENTS)
        return;
    c->extents[c->extent_count++] = (CovExtent){ lo, hi, load_seg, name, overlay };
}

void lift_coverage_unloaded(Machine *m, uint16_t load_seg, uint32_t paragraphs)
{
    Coverage *c = m->coverage;
    if (c)
        drop_range(c, real_addr(load_seg, 0), real_addr(load_seg, 0) + paragraphs * 16);
}

static const CovExtent *innermost(const Coverage *c, uint32_t linear)
{
    const CovExtent *best = NULL;
    for (int i = 0; i < c->extent_count; i++) {
        const CovExtent *e = &c->extents[i];
        if (linear >= e->lo && linear < e->hi && (!best || e->hi - e->lo < best->hi - best->lo))
            best = e;
    }
    return best;
}

static void grow(Coverage *c)
{
    CovEntry *old = c->table;
    size_t old_capacity = c->capacity;
    c->capacity *= 2;
    c->table = calloc(c->capacity, sizeof *c->table);
    if (!c->table)
        fatal("out of memory");
    for (size_t i = 0; i < old_capacity; i++) {
        if (!old[i].key)
            continue;
        size_t j = (size_t)(old[i].key * 0x9E3779B97F4A7C15ull >> 32) & (c->capacity - 1);
        while (c->table[j].key)
            j = (j + 1) & (c->capacity - 1);
        c->table[j] = old[i];
    }
    free(old);
}

void lift_coverage_note(Machine *m, int via)
{
    Coverage *c = m->coverage;
    uint16_t cs = m->cpu.seg[S_CS], ip = (uint16_t)m->cpu.eip;
    uint32_t linear = m->cpu.seg_base[S_CS] + ip;
    uint64_t key = (linear | c->generation << 20) + 1;
    size_t j = (size_t)(key * 0x9E3779B97F4A7C15ull >> 32) & (c->capacity - 1);
    while (c->table[j].key && c->table[j].key != key)
        j = (j + 1) & (c->capacity - 1);
    CovEntry *e = &c->table[j];
    if (!e->key) {
        const CovExtent *x = innermost(c, linear);
        *e = (CovEntry){ .key = key, .name = x ? x->name : -1, .seg = cs, .off = ip };
        if (x && x->overlay) {
            e->seg = 0;
            e->off = (uint16_t)(linear - x->lo);
        } else if (x && cs >= x->load_seg && real_addr(cs, 0) >= x->lo) {
            e->seg = (uint16_t)(cs - x->load_seg);
        } else if (x) { /* reached through a segment below the module: name it by paragraph */
            e->seg = (uint16_t)((linear - x->lo) >> 4);
            e->off = (uint16_t)((linear - x->lo) & 15);
        }
    }
    e->calls++;
    e->via |= (uint8_t)via;
    if (e->calls == 1 && ++c->used * 2 > c->capacity)
        grow(c);
}

static int by_place(const void *a, const void *b)
{
    const CovEntry *x = a, *y = b;
    if (x->name != y->name)
        return x->name < y->name ? -1 : 1;
    if (x->seg != y->seg)
        return x->seg < y->seg ? -1 : 1;
    return (x->off > y->off) - (x->off < y->off);
}

void lift_coverage_write(Machine *m)
{
    Coverage *c = m->coverage;
    if (!c)
        return;
    size_t n = 0;
    for (size_t i = 0; i < c->capacity; i++)
        if (c->table[i].key && c->table[i].name >= 0)
            c->table[n++] = c->table[i];
    qsort(c->table, n, sizeof *c->table, by_place);
    FILE *f = fopen(c->path, "w");
    if (!f) {
        log_msg("coverage: cannot write %s", c->path);
    } else {
        fprintf(f, "# INN coverage: place\tcalls\tvia (module:seg:off; overlays module#stub:off)\n");
        for (size_t i = 0; i < n;) {
            CovEntry sum = c->table[i++];
            while (i < n && !by_place(&sum, &c->table[i])) { /* the same place in several generations */
                sum.calls += c->table[i].calls;
                sum.via |= c->table[i++].via;
            }
            const char *name = c->names[sum.name];
            const char *via = sum.via == (LIFT_COVERAGE_CALL | LIFT_COVERAGE_INT) ? "call,int"
                              : sum.via == LIFT_COVERAGE_INT ? "int" : "call";
            if (strchr(name, '#'))
                fprintf(f, "%s:%04X\t%llu\t%s\n", name, sum.off, (unsigned long long)sum.calls, via);
            else
                fprintf(f, "%s:%04X:%04X\t%llu\t%s\n", name, sum.seg, sum.off, (unsigned long long)sum.calls,
                        via);
        }
        fclose(f);
    }
    free(c->table);
    free(c->path);
    free(c);
    m->coverage = NULL;
}
