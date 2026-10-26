/* Memory and DOS state dumps at fixed points, to compare a hybrid and a native run
 * (tools/m9_dump_diff.py); INN_DUMP_AT and INN_DUMP_DIR choose them (native_exec.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos_int.h"
#include "lift_internal.h"
#include "native_exec.h"

enum { DUMP_BYTES = 0x10FFF0, MAX_SPECS = 16, POINT_LEN = 48, MODULE_LEN = 32, MAX_MCBS = 1024, IVT_ENTRIES = 256 };

typedef struct DumpSpec {
    char point[POINT_LEN];
    int target;  /* occurrence to dump */
    int seen;    /* times the point was reached */
    bool far_call; /* a module address point MODULE:SSSS:OOOO, reached by a far call */
    char module[MODULE_LEN];
    uint16_t seg, off;
} DumpSpec;

static DumpSpec specs[MAX_SPECS];
static int spec_count;
static char parsed_from[512];

/* Parses the environment variable again only when its text changed (tests change it). */
static void parse_specs(const char *env)
{
    if (strcmp(env, parsed_from) == 0)
        return;
    snprintf(parsed_from, sizeof parsed_from, "%s", env);
    spec_count = 0;
    char copy[sizeof parsed_from];
    snprintf(copy, sizeof copy, "%s", env);
    for (char *tok = strtok(copy, ","); tok && spec_count < MAX_SPECS; tok = strtok(NULL, ",")) {
        char *colon = strrchr(tok, ':');
        if (!colon)
            continue;
        *colon = 0;
        DumpSpec *s = &specs[spec_count++];
        snprintf(s->point, sizeof s->point, "%s", tok);
        s->target = atoi(colon + 1);
        s->seen = 0;
        unsigned seg, off;
        char module[MODULE_LEN];
        s->far_call = sscanf(tok, "%31[^:]:%x:%x", module, &seg, &off) == 3;
        if (s->far_call) {
            snprintf(s->module, sizeof s->module, "%s", module);
            s->seg = (uint16_t)seg;
            s->off = (uint16_t)off;
        }
    }
}

bool native_dump_far_call_points(void)
{
    const char *at = getenv("INN_DUMP_AT");
    if (!at || !*at)
        return false;
    parse_specs(at);
    for (int i = 0; i < spec_count; i++)
        if (specs[i].far_call)
            return true;
    return false;
}

void native_dump_far_call(Machine *m)
{
    const char *at = getenv("INN_DUMP_AT");
    if (!at || !*at)
        return;
    parse_specs(at);
    uint32_t linear = m->cpu.seg_base[S_CS] + (m->cpu.eip & 0xFFFF);
    for (int i = 0; i < spec_count; i++) {
        uint16_t base = specs[i].far_call ? lift_module_base(m, specs[i].module) : 0;
        if (base && real_addr((uint16_t)(base + specs[i].seg), specs[i].off) == linear)
            native_dump_point(m, specs[i].point);
    }
}

static void write_registers(Machine *m, FILE *f)
{
    static const char *const NAMES[8] = { "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI" };
    static const char *const SEGS[6] = { "ES", "CS", "SS", "DS", "FS", "GS" };
    fprintf(f, "[cpu]\n");
    for (int i = 0; i < 8; i++)
        fprintf(f, "%s=%04X\n", NAMES[i], m->cpu.r[i].w);
    for (int i = 0; i < 6; i++)
        fprintf(f, "%s=%04X\n", SEGS[i], m->cpu.seg[i]);
    fprintf(f, "IP=%04X\nFLAGS=%04X\n", (unsigned)(m->cpu.eip & 0xFFFF), (unsigned)(m->cpu.flags & 0xFFFF));
}

static void write_mcbs(Machine *m, FILE *f)
{
    fprintf(f, "[mcb] seg owner paras name\n");
    uint16_t mcb = mem_rd16(m, real_addr(DOS_DATA_SEG, DD_FIRST_MCB));
    for (int guard = 0; guard < MAX_MCBS && mcb < DOS_ARENA_END; guard++) {
        uint8_t type = mem_rd8(m, real_addr(mcb, MCB_TYPE));
        if (type != 'M' && type != 'Z') {
            fprintf(f, "%04X damaged type=%02X\n", mcb, type);
            return;
        }
        char name[MCB_NAME_LEN + 1] = { 0 };
        for (int i = 0; i < MCB_NAME_LEN; i++) {
            uint8_t c = mem_rd8(m, real_addr(mcb, (uint16_t)(MCB_NAME + i)));
            name[i] = c >= ' ' && c < 0x7F ? (char)c : (c ? '?' : 0);
        }
        uint16_t paras = mem_rd16(m, real_addr(mcb, MCB_SIZE));
        fprintf(f, "%04X %04X %04X %s\n", mcb, mem_rd16(m, real_addr(mcb, MCB_OWNER)), paras, name);
        if (type == 'Z')
            return;
        mcb = (uint16_t)(mcb + paras + 1);
    }
}

static void write_ivt(Machine *m, FILE *f)
{
    fprintf(f, "[ivt]\n");
    for (int v = 0; v < IVT_ENTRIES; v++)
        fprintf(f, "%02X %04X:%04X\n", v, mem_rd16(m, (uint32_t)v * 4 + 2), mem_rd16(m, (uint32_t)v * 4));
}

static void write_dos(Machine *m, FILE *f)
{
    if (!m->dos)
        return; /* a bare machine (tests) */
    DosState *d = dos_state(m);
    fprintf(f, "[dos]\ncwd=%c:%s\ncur_psp=%04X shell_psp=%04X depth=%d\n", 'A' + d->cur_drive,
            d->cwd, d->cur_psp, d->shell_psp, d->depth);
    fprintf(f, "[sft] slot refs mode path\n");
    for (int i = 0; i < DOS_SFT_MAX; i++)
        if (d->sft[i].refs > 0)
            fprintf(f, "%d %d %02X %s\n", i, d->sft[i].refs, d->sft[i].mode,
                    d->sft[i].dev != DEV_NONE ? "(device)" : d->sft[i].dos_path);
    fprintf(f, "[jft] handle slot (current psp)\n");
    uint32_t psp = real_addr(d->cur_psp, 0);
    uint16_t jft_size = mem_rd16(m, psp + PSP_JFT_SIZE);
    uint32_t jft = real_addr(mem_rd16(m, psp + PSP_JFT_PTR + 2), mem_rd16(m, psp + PSP_JFT_PTR));
    for (uint16_t h = 0; h < jft_size && h < 256; h++) {
        uint8_t slot = mem_rd8(m, jft + h);
        if (slot != JFT_CLOSED)
            fprintf(f, "%u %u\n", h, slot);
    }
}

static void write_dump(Machine *m, const char *dir, const char *point, int n)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/%s-%d.bin", dir, point, n);
    FILE *f = fopen(path, "wb");
    if (!f) {
        log_msg("dump: cannot write %s", path);
        return;
    }
    fwrite(m->mem, 1, DUMP_BYTES, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/%s-%d.txt", dir, point, n);
    f = fopen(path, "w");
    if (!f) {
        log_msg("dump: cannot write %s", path);
        return;
    }
    write_registers(m, f);
    write_mcbs(m, f);
    write_ivt(m, f);
    write_dos(m, f);
    fclose(f);
    log_msg("dump: %s-%d written to %s", point, n, dir);
}

void native_dump_point(Machine *m, const char *point)
{
    const char *at = getenv("INN_DUMP_AT");
    if (!at || !*at)
        return;
    if (m->lift && lift_state(m)->in_shadow)
        return; /* a shadow run repeats the call; only the real one counts */
    parse_specs(at);
    const char *dir = getenv("INN_DUMP_DIR");
    for (int i = 0; i < spec_count; i++) {
        if (strcmp(specs[i].point, point) != 0)
            continue;
        if (++specs[i].seen == specs[i].target)
            write_dump(m, dir && *dir ? dir : ".", point, specs[i].target);
    }
}
