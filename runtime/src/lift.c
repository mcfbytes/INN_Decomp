/* Lifted-function registry and dispatch. Hooks are armed per loaded module and checked against
 * the original bytes at the entry, so a different build of the same module is never patched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bios_internal.h"
#include "lift_internal.h"
#include "native.h"
#include "native_exec.h"

void lift_set_hook_bit(Machine *m, uint32_t linear, bool on)
{
    if (linear >= MEM_ONE_MEG)
        return;
    if (on)
        m->exec_hooks[linear >> 3] |= (uint8_t)(1u << (linear & 7));
    else
        m->exec_hooks[linear >> 3] &= (uint8_t)~(1u << (linear & 7));
}

/* The far-return sentinel: guest code called by lifted code returns into this stub. */
static CallbackResult far_return_reached(Machine *m)
{
    LiftState *s = lift_state(m);
    if (s->nesting > 0)
        s->nested[s->nesting - 1].returned = true;
    m->cpu.stop = true;
    return CB_DONE;
}

void lift_init(Machine *m)
{
    m->lift = calloc(1, sizeof(LiftState));
    m->exec_hooks = calloc(1, HOOK_BITMAP_BYTES);
    if (!m->lift || !m->exec_hooks)
        fatal("out of memory");
    lift_state(m)->return_callback = callback_register(m, "lift far return", far_return_reached, -1);
    lifted_register_all(m);
    lift_state(m)->only = getenv("INN_LIFT_ONLY");
    lift_state(m)->verify_skip = getenv("INN_VERIFY_SKIP");
    lift_state(m)->verify_replay = getenv("INN_VERIFY_REPLAY");
    const char *verify_calls = getenv("INN_VERIFY_LIFTED");
    if (verify_calls && atoi(verify_calls) > 0)
        lift_verify_enable(m, atoi(verify_calls));
    const char *coverage = getenv("INN_COVERAGE");
    if (coverage && *coverage)
        lift_coverage_enable(m, coverage);
    const char *direct = getenv("INN_DIRECT_CALLS");
    bool direct_all = !direct || !*direct || *direct == '1';
    lift_state(m)->direct = direct_all || (direct && *direct == '2');
    lift_state(m)->direct_cb = direct_all || (direct && *direct == '3');
    const char *native_control = getenv("INN_NATIVE_CONTROL");
    lift_state(m)->native_control = native_control && strcmp(native_control, "1") == 0;
    const char *prof = getenv("INN_INTERP_PROFILE");
    if (prof && *prof) {
        dosrt_interp_profile = calloc(MEM_ONE_MEG / 16, sizeof *dosrt_interp_profile);
    }
    const char *synth = getenv("INN_SYNTH");
    if (synth && *synth)
        lift_synth_load(m, synth);
    lift_state(m)->dump_far_calls = native_dump_far_call_points();
    lift_update_probe(m);
}

static void log_verify_line(const LiftedEntry *e, const HookStats *st)
{
    if (st->replay_calls) {
        if (st->result_diffs)
            log_msg("verify: %s %s: %d calls checked, %d mismatches, DX:AX differs in %d (device replay)", e->module,
                    e->name, st->shadow_calls, st->shadow_mismatches, st->result_diffs);
        else
            log_msg("verify: %s %s: %d calls checked, %d mismatches (device replay)", e->module, e->name,
                    st->shadow_calls, st->shadow_mismatches);
        return;
    }
    if (e->no_shadow) { /* live calls are not checked; synthetic ones (call!) are, below */
        if (st->armed_once)
            log_msg("verify: %s %s: not shadowed (LIFT_NOSHADOW)", e->module, e->name);
    } else if (st->armed_once && st->result_diffs)
        log_msg("verify: %s %s: %d calls checked, %d mismatches, DX:AX differs in %d", e->module, e->name,
                st->shadow_calls, st->shadow_mismatches, st->result_diffs);
    else if (st->armed_once)
        log_msg("verify: %s %s: %d calls checked, %d mismatches", e->module, e->name, st->shadow_calls,
                st->shadow_mismatches);
    if (st->synth_calls && st->synth_result_diffs)
        log_msg("verify: %s %s: %d calls checked, %d mismatches, DX:AX differs in %d (synthetic)", e->module,
                e->name, st->synth_calls, st->synth_mismatches, st->synth_result_diffs);
    else if (st->synth_calls)
        log_msg("verify: %s %s: %d calls checked, %d mismatches (synthetic)", e->module, e->name,
                st->synth_calls, st->synth_mismatches);
}

/* Every entry that was armed at some point, loaded now or not, and every one called synthetically. */
static void log_verify_summary(LiftState *s)
{
    for (int t = 0; t < s->table_count; t++)
        for (int i = 0; i < s->tables[t].count; i++)
            if (s->tables[t].stats[i].armed_once || s->tables[t].stats[i].synth_calls)
                log_verify_line(&s->tables[t].entries[i], &s->tables[t].stats[i]);
}

/* Where the interpreter still ran, by 16-byte paragraph, with the modules' load segments. */
static void write_direct_counts(FILE *f, const LiftState *s)
{
    fprintf(f, "direct %llu fallbacks %llu callbacks %llu thunks %llu returns %llu\n",
            (unsigned long long)s->direct_calls, (unsigned long long)s->direct_fallbacks,
            (unsigned long long)s->direct_callbacks, (unsigned long long)s->direct_thunks,
            (unsigned long long)s->direct_returns);
}

static void write_interp_profile(Machine *m)
{
    const char *path = getenv("INN_INTERP_PROFILE");
    if (!dosrt_interp_profile || !path)
        return;
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    LiftState *s = lift_state(m);
    for (int i = 0; s && i < s->module_count; i++)
        fprintf(f, "module %s %04X\n", s->modules[i].name, s->modules[i].load_seg);
    if (s)
        write_direct_counts(f, s);
    for (uint32_t p = 0; p < MEM_ONE_MEG / 16; p++)
        if (dosrt_interp_profile[p])
            fprintf(f, "%05X %u\n", p, dosrt_interp_profile[p]);
    fclose(f);
}

void lift_shutdown(Machine *m)
{
    write_interp_profile(m);
    if (m->lift && getenv("INN_DIRECT_STATS"))
        write_direct_counts(stderr, lift_state(m));
    lift_coverage_write(m);
    if (m->lift && lift_state(m)->verify)
        log_verify_summary(lift_state(m));
    if (m->lift)
        lift_synth_shutdown(m);
    if (m->lift) {
        free(lift_state(m)->verify);
        lift_replay_free(m);
        for (int t = 0; t < lift_state(m)->table_count; t++)
            free(lift_state(m)->tables[t].stats);
    }
    free(m->lift);
    free(m->exec_hooks);
    m->lift = NULL;
    m->exec_hooks = NULL;
}

void lift_add_table(Machine *m, const LiftedEntry *entries, int count)
{
    LiftState *s = lift_state(m);
    if (s->table_count >= MAX_TABLES)
        fatal("too many lifted-function tables");
    s->tables[s->table_count].entries = entries;
    s->tables[s->table_count].stats = calloc((size_t)(count > 0 ? count : 1), sizeof(HookStats));
    if (!s->tables[s->table_count].stats)
        fatal("out of memory");
    s->tables[s->table_count].count = count;
    s->table_count++;
}

void lift_add_module(Machine *m, const LiftedModule *module)
{
    LiftState *s = lift_state(m);
    if (s->module_meta_count >= MAX_MODULE_META)
        fatal("too many lifted module images");
    s->module_meta[s->module_meta_count++] = module;
}

static uint64_t image_hash(const uint8_t *p, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ULL; /* FNV-1a, as in tools/gen_lift_table.py */
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ULL;
    return h;
}

/* Whether this module name has a recorded build image at all (tests and images loaded by a program do not). */
static bool module_has_meta(const LiftState *s, const char *module)
{
    for (int i = 0; i < s->module_meta_count; i++)
        if (!strcmp(s->module_meta[i]->name, module))
            return true;
    return false;
}

static bool module_image_matches(const LiftState *s, const char *module, uint32_t bytes, uint64_t hash)
{
    for (int i = 0; i < s->module_meta_count; i++) {
        const LiftedModule *meta = s->module_meta[i];
        if (!strcmp(meta->name, module) && meta->image_bytes == bytes && meta->image_hash == hash)
            return true;
    }
    return false;
}

/* Whether the module at load_seg is the build its tables describe; true for modules without a recorded image. */
static bool module_build_matches(const LiftState *s, const char *module, uint16_t load_seg)
{
    for (int i = 0; i < s->module_count; i++)
        if (s->modules[i].load_seg == load_seg && !strcmp(s->modules[i].name, module))
            return s->modules[i].build_matches;
    return true;
}

static void remember_module(LiftState *s, const char *module, uint16_t load_seg);

static void set_module_build(LiftState *s, const char *module, uint16_t load_seg, bool matches)
{
    for (int i = 0; i < s->module_count; i++)
        if (s->modules[i].load_seg == load_seg && !strcmp(s->modules[i].name, module)) {
            s->modules[i].build_matches = matches;
            return;
        }
}

static int by_linear(const void *a, const void *b)
{
    const ActiveHook *x = a, *y = b;
    return x->linear < y->linear ? -1 : x->linear > y->linear;
}

/* Whether name is one of the comma-separated names in list. */
bool lift_listed(const char *list, const char *name)
{
    size_t len = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += len)
        if ((p == list || p[-1] == ',') && (p[len] == ',' || p[len] == 0))
            return true;
    return false;
}

static void arm(Machine *m, const LiftedEntry *e, HookStats *stats, uint16_t load_seg, uint16_t reloc_base,
                bool overlay)
{
    LiftState *s = lift_state(m);
    if (s->only && !lift_listed(s->only, e->name))
        return;
    if (m->native)
        s->native_seen = true;
    if (e->native_only && !s->native_control && !s->native_seen)
        return;
    if (s->active_count >= MAX_ACTIVE)
        fatal("too many active lifted functions");
    uint32_t linear = real_addr((uint16_t)(load_seg + e->seg), e->off);
    s->active[s->active_count++] = (ActiveHook){ .linear = linear, .load_seg = load_seg,
                                                 .reloc_base = reloc_base, .entry = e, .overlay = overlay,
                                                 .stats = stats };
    stats->armed_once = true;
    lift_set_hook_bit(m, linear, true);
}

/* Arms every entry of module at load_seg, replacing whatever was hooked in its image. */
static void arm_module(Machine *m, const char *module, uint16_t load_seg, uint32_t image_bytes,
                       uint16_t reloc_base, bool overlay)
{
    LiftState *s = lift_state(m);
    lift_module_unloaded(m, load_seg, (image_bytes + 15) / 16);
    remember_module(s, module, load_seg);
    set_module_build(s, module, load_seg, true);
    for (int t = 0; t < s->table_count; t++)
        for (int i = 0; i < s->tables[t].count; i++)
            if (!strcmp(s->tables[t].entries[i].module, module))
                arm(m, &s->tables[t].entries[i], &s->tables[t].stats[i], load_seg, reloc_base, overlay);
    qsort(s->active, (size_t)s->active_count, sizeof s->active[0], by_linear);
}

/* Drops the hooks of one module wherever they are armed. */
static void disarm_module(Machine *m, const char *module)
{
    LiftState *s = lift_state(m);
    int kept = 0;
    for (int i = 0; i < s->active_count; i++) {
        ActiveHook *h = &s->active[i];
        if (!strcmp(h->entry->module, module))
            lift_set_hook_bit(m, h->linear, false);
        else
            s->active[kept++] = *h;
    }
    s->active_count = kept;
}

static void remember_module(LiftState *s, const char *module, uint16_t load_seg)
{
    int i = 0;
    while (i < s->module_count && strcmp(s->modules[i].name, module) != 0)
        i++;
    if (i == s->module_count) {
        if (s->module_count == (int)(sizeof s->modules / sizeof s->modules[0]))
            return;
        s->module_count++;
        snprintf(s->modules[i].name, sizeof s->modules[i].name, "%s", module);
        s->modules[i].build_matches = true;
    }
    s->modules[i].load_seg = load_seg;
}

const char *lift_module_owning(Machine *m, uint16_t seg, uint16_t *load_seg)
{
    LiftState *s = lift_state(m);
    const char *best = NULL;
    *load_seg = 0;
    for (int i = 0; s && i < s->module_count; i++)
        if (s->modules[i].load_seg <= seg && (!best || s->modules[i].load_seg > *load_seg)) {
            best = s->modules[i].name;
            *load_seg = s->modules[i].load_seg;
        }
    return best;
}

bool lift_module_has_native(Machine *m, const char *module)
{
    LiftState *s = lift_state(m);
    for (int t = 0; s && t < s->table_count; t++)
        for (int i = 0; i < s->tables[t].count; i++)
            if (s->tables[t].entries[i].native_only && !strcmp(s->tables[t].entries[i].module, module))
                return true;
    return false;
}

uint16_t lift_module_base(Machine *m, const char *module)
{
    LiftState *s = lift_state(m);
    for (int i = 0; s && i < s->module_count; i++)
        if (!strcmp(s->modules[i].name, module))
            return s->modules[i].load_seg;
    return 0;
}

/* ---- images the program loads by itself (docs/lifting.md, "Images loaded by the program") ---- */

/* The far call probe costs a test per far call only while an image waits or a dump point is set. */
void lift_update_probe(Machine *m)
{
    LiftState *s = lift_state(m);
    bool waiting = s->dump_far_calls;
    for (int i = 0; i < s->image_count && !waiting; i++)
        waiting = s->images[i].load_seg == 0;
    m->far_call_probe = waiting;
}

static int image_index(const LiftState *s, const char *module)
{
    for (int i = 0; i < s->image_count; i++)
        if (!strcmp(s->images[i].image->module, module))
            return i;
    return -1;
}

void lift_add_image(Machine *m, const LiftedImage *image)
{
    LiftState *s = lift_state(m);
    if (s->image_count == MAX_IMAGES)
        fatal("too many lifted images");
    s->images[s->image_count].image = image;
    s->images[s->image_count++].load_seg = 0;
    lift_update_probe(m);
}

static bool image_head_at(Machine *m, const LiftedImage *image, uint32_t linear)
{
    for (int i = 0; i < LIFT_IMAGE_HEAD_LEN; i++)
        if ((image->head_mask >> i & 1) && mem_rd8(m, linear + (uint32_t)i) != image->head[i])
            return false;
    return true;
}

/* Whether seg is where a program, driver or announced image starts: its offset 0 is that module's. */
static bool is_module_start(const LiftState *s, uint16_t seg)
{
    for (int i = 0; i < s->image_count; i++)
        if (s->images[i].load_seg == seg)
            return true;
    for (int i = 0; i < s->module_count; i++)
        if (s->modules[i].load_seg == seg && image_index(s, s->modules[i].name) < 0)
            return true;
    return false;
}

void lift_far_call_probe(Machine *m)
{
    LiftState *s = lift_state(m);
    if (!s)
        return;
    if (s->dump_far_calls)
        native_dump_far_call(m);
    if (s->in_shadow)
        return; /* arming a module is not part of the shadow snapshot */
    uint16_t seg = m->cpu.seg[S_CS];
    if ((m->cpu.eip & 0xFFFF) != 0 || s->image_count == 0 || is_module_start(s, seg))
        return;
    uint32_t linear = real_addr(seg, 0);
    for (int i = 0; i < s->image_count; i++) {
        const LiftedImage *image = s->images[i].image;
        if (linear + image->bytes > MEM_ONE_MEG || !image_head_at(m, image, linear))
            continue;
        if (s->images[i].load_seg)
            disarm_module(m, image->module); /* the program loaded it again elsewhere */
        log_msg("lift: image %s found at %04X (%u bytes)", image->module, seg, image->bytes);
        lift_module_loaded(m, image->module, seg, image->bytes);
        return;
    }
}

void lift_module_loaded(Machine *m, const char *module, uint16_t load_seg, uint32_t image_bytes)
{
    LiftState *s = lift_state(m);
    if (!s)
        return;
    remember_module(s, module, load_seg);
    set_module_build(s, module, load_seg, true);
    if (m->trace)
        log_msg("lift: module %s at %04X (%u bytes)", module, load_seg, image_bytes);
    int image = image_index(s, module);
    if (image >= 0 && s->images[image].load_seg && s->images[image].load_seg != load_seg)
        disarm_module(m, module); /* the image moved: old hooks point into freed memory */
    arm_module(m, module, load_seg, image_bytes, load_seg, false);
    lift_coverage_module(m, module, load_seg, image_bytes, false);
    lift_synth_module(m, module, load_seg);
    if (image >= 0) { /* a lifted loader may announce an image itself */
        s->images[image].load_seg = load_seg;
        lift_update_probe(m);
    }
}

bool lift_module_loaded_image(Machine *m, const char *module, uint16_t load_seg, const uint8_t *image,
                              uint32_t image_bytes)
{
    LiftState *s = lift_state(m);
    if (!s)
        return false;
    uint64_t hash = image_hash(image, image_bytes);
    if (module_has_meta(s, module) && !module_image_matches(s, module, image_bytes, hash)) {
        if (module_build_matches(s, module, load_seg))
            log_msg("lift: %s at %04X is a different build (%u bytes, hash %016llX); running its original code",
                    module, load_seg, image_bytes, (unsigned long long)hash);
        lift_module_unloaded(m, load_seg, (image_bytes + 15) / 16);
        remember_module(s, module, load_seg);
        set_module_build(s, module, load_seg, false);
        lift_coverage_module(m, module, load_seg, image_bytes, false);
        return false;
    }
    lift_module_loaded(m, module, load_seg, image_bytes);
    return true;
}

void lift_overlay_loaded(Machine *m, const char *module, uint16_t load_seg, uint32_t image_bytes,
                         uint16_t reloc_base)
{
    LiftState *s = lift_state(m);
    if (!s)
        return;
    /* Overlay code comes from the program's own EXE: a program that is another build had its
     * resident entries rejected, and its overlays go with them. */
    const char *owner = module;
    char program[16];
    const char *hash = strchr(module, '#');
    if (hash && (size_t)(hash - module) < sizeof program) {
        memcpy(program, module, (size_t)(hash - module));
        program[hash - module] = 0;
        owner = program;
    }
    if (!module_build_matches(s, owner, reloc_base))
        return;
    if (m->trace)
        log_msg("lift: overlay %s at %04X (%u bytes)", module, load_seg, image_bytes);
    disarm_module(m, module);
    arm_module(m, module, load_seg, image_bytes, reloc_base, true);
    lift_coverage_module(m, module, load_seg, image_bytes, true);
    lift_synth_module(m, module, load_seg);
}

void lift_module_unloaded(Machine *m, uint16_t load_seg, uint32_t paragraphs)
{
    LiftState *s = lift_state(m);
    if (!s)
        return;
    lift_coverage_unloaded(m, load_seg, paragraphs);
    uint32_t lo = real_addr(load_seg, 0), hi = lo + paragraphs * 16;
    int kept = 0;
    for (int i = 0; i < s->active_count; i++) {
        ActiveHook *h = &s->active[i];
        if (h->linear >= lo && h->linear < hi)
            lift_set_hook_bit(m, h->linear, false);
        else
            s->active[kept++] = *h;
    }
    s->active_count = kept;
    lift_synth_unloaded(m, lo, hi);
    for (int i = 0; i < s->module_count; i++)
        if (s->modules[i].load_seg && real_addr(s->modules[i].load_seg, 0) >= lo &&
            real_addr(s->modules[i].load_seg, 0) < hi) {
            s->modules[i].load_seg = 0; /* the segment is free; a later load there is new */
            s->modules[i].build_matches = true;
        }
    bool images_gone = false;
    for (int i = 0; i < s->image_count; i++) {
        uint32_t at = real_addr(s->images[i].load_seg, 0);
        if (s->images[i].load_seg && at >= lo && at < hi) {
            s->images[i].load_seg = 0; /* the next load is found again */
            images_gone = true;
        }
    }
    if (images_gone)
        lift_update_probe(m);
}

static ActiveHook *find_hook(LiftState *s, uint32_t linear)
{
    ActiveHook key = { .linear = linear };
    return bsearch(&key, s->active, (size_t)s->active_count, sizeof s->active[0], by_linear);
}

/* The signature as it sits in memory once the loader added the load segment to relocated words. */
static void expected_bytes(const ActiveHook *h, uint8_t out[LIFT_SIGNATURE_LEN], bool skip[LIFT_SIGNATURE_LEN])
{
    const LiftedEntry *e = h->entry;
    memcpy(out, e->signature, LIFT_SIGNATURE_LEN);
    for (int i = 0; i < LIFT_SIGNATURE_LEN; i++)
        skip[i] = (e->unknown_mask >> i & 1) != 0;
    skip[0] |= (e->reloc_mask & LIFT_RELOC_SPLIT_HEAD) != 0;
    for (int i = 0; i < LIFT_SIGNATURE_LEN; i++) {
        if (!(e->reloc_mask >> i & 1))
            continue;
        uint16_t word = (uint16_t)(e->signature[i] | (i + 1 < LIFT_SIGNATURE_LEN ? e->signature[i + 1] << 8 : 0));
        word = (uint16_t)(word + h->reloc_base);
        out[i] = (uint8_t)word;
        if (i + 1 < LIFT_SIGNATURE_LEN)
            out[i + 1] = (uint8_t)(word >> 8);
    }
}

/* ---- floating-point emulator sequences ---- */

static const uint8_t EMU_SEG_PREFIX[4] = { 0x3E, 0x2E, 0x36, 0x26 }; /* b >> 6 of CD 3C b: DS, CS, SS, ES */

int lift_emu_int_len(const uint8_t *b, int n)
{
    if (n < 2 || b[0] != 0xCD || b[1] < 0x34 || b[1] > 0x3D)
        return 0;
    int len = b[1] == 0x3C ? 3 : 2;
    return len <= n ? len : 0;
}

int lift_emu_to_int(const uint8_t *b, int n, uint8_t out[3])
{
    int len = lift_emu_int_len(b, n);
    if (len) {
        memcpy(out, b, (size_t)len);
        return len;
    }
    if (n >= 2 && b[0] == 0x90 && b[1] == 0x9B) {
        out[0] = 0xCD, out[1] = 0x3D;
        return 2;
    }
    if (n < 2 || b[0] != 0x9B)
        return 0;
    if (b[1] >= 0xD8 && b[1] <= 0xDF) {
        out[0] = 0xCD, out[1] = (uint8_t)(0x34 + b[1] - 0xD8);
        return 2;
    }
    for (int s = 0; s < 4; s++)
        if (n >= 3 && b[1] == EMU_SEG_PREFIX[s] && b[2] >= 0xD8 && b[2] <= 0xDF) {
            out[0] = 0xCD, out[1] = 0x3C, out[2] = (uint8_t)(s << 6 | (b[2] & 0x3F));
            return 3;
        }
    return 0;
}

bool lift_emu_is_patch_of(const uint8_t *intform, const uint8_t *patched, int len)
{
    if (lift_emu_int_len(intform, len) != len || patched[0] == 0xCD)
        return false;
    uint8_t back[3];
    return lift_emu_to_int(patched, len, back) == len && memcmp(back, intform, (size_t)len) == 0;
}

bool lift_entry_bytes_match(Machine *m, const ActiveHook *h)
{
    uint8_t expected[LIFT_SIGNATURE_LEN];
    bool skip[LIFT_SIGNATURE_LEN];
    expected_bytes(h, expected, skip);
    for (int i = 0; i < LIFT_SIGNATURE_LEN; i++) {
        if (skip[i] || mem_rd8(m, h->linear + (uint32_t)i) == expected[i])
            continue;
        if (!(h->entry->emu_mask >> i & 1))
            return false;
        /* An emulator sequence: the file and memory may each hold the INT or the patched form. */
        uint8_t have[3], want[3], seen[3];
        for (int k = 0; k < 3; k++)
            seen[k] = mem_rd8(m, h->linear + (uint32_t)(i + k));
        int len = lift_emu_to_int(expected + i, LIFT_SIGNATURE_LEN - i, want);
        if (!len || lift_emu_to_int(seen, 3, have) != len || memcmp(have, want, (size_t)len) != 0)
            return false;
        i += len - 1;
    }
    return true;
}

static bool signature_matches(Machine *m, ActiveHook *h)
{
    if (h->overlay) {
        /* The buffer is reused, and a manager may move code without a file read we would see. */
        bool ok = lift_entry_bytes_match(m, h);
        if (!ok && !h->absent_logged) {
            h->absent_logged = true;
            log_msg("lift: %s %s reached with other bytes at the entry; running the original",
                    h->entry->module, h->entry->name);
        }
        return ok;
    }
    if (!h->verified) {
        h->verified = true;
        h->rejected = !lift_entry_bytes_match(m, h);
        if (h->rejected)
            log_msg("lift: %s %s bytes differ at the entry; running the original", h->entry->module,
                    h->entry->name);
    }
    return !h->rejected;
}

/* A near return into a hooked entry with the expected SP ends the innermost nested call. */
static bool nested_return_reached(Machine *m, LiftState *s, uint32_t linear)
{
    if (s->nesting == 0)
        return false;
    NestedCall *n = &s->nested[s->nesting - 1];
    uint16_t sp = m->cpu.r[R_SP].w;
    if (n->return_linear != linear || (n->sp_above ? (uint16_t)(sp - n->return_sp - 1) >= 0x8000 : sp != n->return_sp))
        return false;
    n->returned = true;
    m->cpu.stop = true;
    return true;
}

bool lift_dispatch(Machine *m, uint32_t linear)
{
    LiftState *s = lift_state(m);
    if (nested_return_reached(m, s, linear))
        return true;
    if (s->hooks_off)
        return false;
    if (s->synth && !s->in_shadow)
        lift_synth_reached(m, linear);
    ActiveHook *h = find_hook(s, linear);
    if (!h || !signature_matches(m, h))
        return false;
    if (!lift_verify_call(m, h)) {
        if (s->in_shadow)
            lift_verify_nested_entry(m);
        lift_call_c(m, h->entry->fn);
    }
    return true;
}

void lift_call_c(Machine *m, LiftedFn fn)
{
    lift_state(m)->c_depth++;
    fn(m);
    lift_state(m)->c_depth--;
}

bool lift_in_c(const Machine *m)
{
    return m->lift && ((const LiftState *)m->lift)->c_depth > 0;
}

void lift_set_nested_yield(Machine *m, void (*fn)(Machine *m))
{
    lift_state(m)->nested_yield = fn;
}

void lift_run_nested(Machine *m, uint32_t return_linear, uint16_t return_sp, bool sp_above)
{
    LiftState *s = lift_state(m);
    if (s->nesting >= MAX_NESTING)
        fatal("lifted code nested too deeply");
    NestedCall *n = &s->nested[s->nesting++];
    *n = (NestedCall){ .return_linear = return_linear, .return_sp = return_sp, .sp_above = sp_above };
    uint64_t start = m->cpu.instructions, stall_report = start + SHADOW_STALL_INSNS;
    int c_depth = s->c_depth;
    s->c_depth = 0;
    while (!n->returned && !m->exit_requested && !s->nested_abort) {
        machine_step(m);
        if (s->nested_budget && m->cpu.instructions >= s->nested_budget)
            s->nested_abort = true;
        if (m->native) /* the non-strict fallback: interrupts and yields come from the native core */
            native_safepoint(m);
        else if (s->nested_yield && !s->in_shadow)
            s->nested_yield(m);
        if (s->shadow_of && m->cpu.instructions >= stall_report) {
            /* Interrupts are held during a shadow run, so code that waits on a timer tick spins. */
            log_msg("verify: shadow run of %s still running after %llu instructions, at %04X:%04X",
                    s->shadow_of->entry->name, (unsigned long long)(m->cpu.instructions - start),
                    m->cpu.seg[S_CS], (unsigned)(m->cpu.eip & 0xFFFF));
            stall_report += SHADOW_STALL_INSNS;
        }
    }
    s->c_depth = c_depth;
    s->nesting--;
}

/* An interrupt the CPU would take at the callee's first instruction boundary. Native mode delivers
 * interrupts at the safe point instead. */
static bool irq_due(const Machine *m)
{
    return !m->native && (m->cpu.flags & F_IF) && m->irq_pending && !m->irq_hold && !m->irq_gate;
}

/* The CPU path cost at least the hooked instruction and the return stub; a call that costs no time
 * breaks scripts that time a loop (a speed test divides by the ticks it took). Native mode charges
 * at the safe point. */
static void charge_direct(Machine *m)
{
    if (!m->native)
        machine_charge_c(m, m->io_cost_ns);
}

/* Runs the lifted function at linear as a plain C call, as lift_dispatch would from the CPU loop;
 * false when the target is not an active lifted entry (the CPU must run it). */
static bool call_lifted_direct(Machine *m, LiftState *s, uint32_t linear)
{
    if (linear >= MEM_ONE_MEG || !lift_hooked(m, linear))
        return false;
    if (irq_due(m))
        return false; /* the CPU takes it at the entry's instruction boundary, as before */
    if (s->synth && !s->in_shadow)
        lift_synth_reached(m, linear);
    ActiveHook *h = find_hook(s, linear);
    if (!h || !signature_matches(m, h))
        return false;
    s->direct_calls++;
    charge_direct(m);
    if (!lift_verify_call(m, h)) {
        if (s->in_shadow)
            lift_verify_nested_entry(m);
        lift_call_c(m, h->entry->fn);
    }
    return true;
}

static bool is_service_stub(const Machine *m, uint32_t linear)
{
    if (linear < callback_stub_addr(0) || linear >= callback_stub_addr(CALLBACK_MAX))
        return false;
    const uint8_t *b = m->mem + linear;
    return b[0] == 0xFE && b[1] == 0x38 && (b[3] == 0xCF || b[3] == 0xCB);
}

/* A host service stub (FE 38 id CF/CB) called from lifted code runs its C directly. */
static bool call_callback_direct(Machine *m, LiftState *s, uint32_t linear)
{
    if (m->io_mode != IO_LIVE || m->irq_gate)
        return false;
    if (!m->native && (!s->direct_cb || m->trace))
        return false;
    /* The caller's IF is in the frame guest_int pushed; a due IRQ goes through the CPU path. */
    uint16_t caller_flags = mem_rd16(m, m->cpu.seg_base[S_SS] + (uint16_t)(m->cpu.r[R_SP].w + 4));
    if (!m->native && (caller_flags & F_IF) && m->irq_pending && !m->irq_hold)
        return false;
    if (!is_service_stub(m, linear))
        return false;
    const uint8_t *b = m->mem + linear;
    if (b[2] >= m->callback_count)
        return false;
    if ((int)b[2] == s->return_callback || !m->callbacks[b[2]])
        return false;
    charge_direct(m);
    m->cpu.eip = (m->cpu.eip + 3) & 0xFFFF;
    while (m->callbacks[b[2]](m) == CB_RETRY) {
        if (!m->native) {
            m->cpu.eip = (m->cpu.eip - 3) & 0xFFFF; /* the CPU retries the stub */
            return false;
        }
        native_idle(m); /* a blocked service waits for the next interrupt, then asks again */
    }
    s->direct_callbacks++;
    uint16_t ip = cpu_pop16(m), cs = cpu_pop16(m);
    if (b[3] == 0xCF) {
        uint16_t flags = cpu_pop16(m);
        m->cpu.flags = (m->cpu.flags & 0xFFFF0000u) | (flags & 0x7FD5u) | 2u;
    }
    cpu_set_seg(&m->cpu, S_CS, cs);
    m->cpu.eip = ip;
    return true;
}

/* Jump thunks (a driver's entry JMPs to the body) and bare returns are followed in C; ROM is not. */
enum { MAX_THUNK_HOPS = 4, DIRECT_ROM_LIMIT = 0xF0000, THUNK_PEEK = 5 };

typedef enum { TARGET_OTHER, TARGET_JUMP, TARGET_RETURN } TargetKind;

/* What the bytes at linear are: a jump (dest set) or a trivial return, else other. */
static TargetKind classify_target(Machine *m, uint32_t cs_base, uint16_t ip, uint32_t linear, uint32_t *dest_linear,
                                  uint16_t *dest_cs)
{
    if (linear >= DIRECT_ROM_LIMIT)
        return TARGET_OTHER;
    uint8_t b[THUNK_PEEK];
    for (int i = 0; i < THUNK_PEEK; i++)
        b[i] = mem_rd8(m, linear + (uint32_t)i);
    *dest_cs = (uint16_t)(cs_base >> 4);
    uint16_t dest_ip;
    switch (b[0]) {
    case 0xE9:
        dest_ip = (uint16_t)(ip + 3 + (uint16_t)(b[1] | b[2] << 8));
        break;
    case 0xEB:
        dest_ip = (uint16_t)(ip + 2 + (int8_t)b[1]);
        break;
    case 0xEA:
        dest_ip = (uint16_t)(b[1] | b[2] << 8);
        *dest_cs = (uint16_t)(b[3] | b[4] << 8);
        break;
    case 0xC3: case 0xCB: case 0xC2: case 0xCA: case 0xCF:
        return TARGET_RETURN;
    default:
        return TARGET_OTHER;
    }
    *dest_linear = ((uint32_t)*dest_cs << 4) + dest_ip;
    return TARGET_JUMP;
}

/* Pops the return a trivial-return instruction at CS:IP performs. */
static void apply_trivial_return(Machine *m, uint8_t op, uint16_t arg_bytes)
{
    uint16_t ip = cpu_pop16(m);
    if (op == 0xCB || op == 0xCA || op == 0xCF) {
        uint16_t cs = cpu_pop16(m);
        cpu_set_seg(&m->cpu, S_CS, cs);
    }
    if (op == 0xCF) {
        uint16_t flags = cpu_pop16(m);
        m->cpu.flags = (m->cpu.flags & 0xFFFF0000u) | (flags & 0x7FD5u) | 2u;
    }
    m->cpu.eip = ip;
    m->cpu.r[R_SP].w += arg_bytes;
}

typedef enum { THUNK_NONE, THUNK_MOVED, THUNK_RETURNED } ThunkResult;

/* Follows jump thunks from CS:IP while the destination is hooked or again a thunk or return, then
 * applies a trivial return found there. MOVED: CS:IP is the thunk's destination, which still has to
 * run. RETURNED: the callee was a bare return and has returned. */
static ThunkResult follow_thunks(Machine *m, LiftState *s)
{
    ThunkResult moved = THUNK_NONE;
    for (int hop = 0; hop <= MAX_THUNK_HOPS; hop++) {
        uint16_t ip = (uint16_t)m->cpu.eip;
        uint32_t linear = m->cpu.seg_base[S_CS] + ip, dest_linear;
        uint16_t dest_cs;
        TargetKind kind = classify_target(m, m->cpu.seg_base[S_CS], ip, linear, &dest_linear, &dest_cs);
        if (kind == TARGET_RETURN) {
            uint8_t op = mem_rd8(m, linear);
            uint16_t arg = (op == 0xC2 || op == 0xCA) ? mem_rd16(m, linear + 1) : 0;
            charge_direct(m);
            apply_trivial_return(m, op, arg);
            s->direct_returns++;
            return THUNK_RETURNED;
        }
        if (kind != TARGET_JUMP || hop == MAX_THUNK_HOPS)
            return moved;
        uint32_t ignore_linear;
        uint16_t ignore_cs;
        uint16_t dest_ip = (uint16_t)(dest_linear - ((uint32_t)dest_cs << 4));
        if (dest_linear >= DIRECT_ROM_LIMIT)
            return moved;
        if (!lift_hooked(m, dest_linear) &&
            classify_target(m, (uint32_t)dest_cs << 4, dest_ip, dest_linear, &ignore_linear, &ignore_cs) == TARGET_OTHER)
            return moved;
        charge_direct(m);
        cpu_set_seg(&m->cpu, S_CS, dest_cs);
        m->cpu.eip = dest_ip;
        s->direct_thunks++;
        moved = THUNK_MOVED;
    }
    return moved;
}

/* Native mode (docs/native.md sections 6 and 7): the call is a safe point, and what no lifted entry,
 * thunk, return or service stub resolves is a ROM lift, the unresolved hook, or an error. */
static bool call_native(Machine *m, LiftState *s)
{
    native_safepoint(m);
    if (follow_thunks(m, s) == THUNK_RETURNED)
        return true;
    uint32_t linear = m->cpu.seg_base[S_CS] + (m->cpu.eip & 0xFFFF);
    if (call_lifted_direct(m, s, linear) || call_callback_direct(m, s, linear))
        return true;
    if (s->unresolved && !is_service_stub(m, linear) && s->unresolved(m, linear))
        return true;
    return native_unresolved(m, linear);
}

/* Dispatches the call whose callee is at CS:IP without the CPU where it can. True when something ran
 * or moved CS:IP; the caller then checks whether the return address was reached. */
static bool call_direct(Machine *m, LiftState *s)
{
    if (s->hooks_off)
        return false;
    if (m->native)
        return call_native(m, s);
    ThunkResult thunks = THUNK_NONE;
    if (s->direct && !irq_due(m))
        thunks = follow_thunks(m, s);
    if (thunks == THUNK_RETURNED)
        return true;
    uint32_t linear = m->cpu.seg_base[S_CS] + (m->cpu.eip & 0xFFFF);
    if (s->direct && call_lifted_direct(m, s, linear))
        return true;
    if (call_callback_direct(m, s, linear))
        return true;
    if (s->direct && s->unresolved && !irq_due(m) && !is_service_stub(m, linear) && s->unresolved(m, linear))
        return true;
    /* The charge for a followed jump can make an interrupt due; the CPU then takes it at the
     * destination, which is not a failed direct call. */
    return thunks == THUNK_MOVED && !irq_due(m);
}

void lift_set_unresolved(Machine *m, LiftUnresolvedFn fn)
{
    lift_state(m)->unresolved = fn;
}

void lift_dispatch_far_at(Machine *m, uint16_t seg, uint16_t off)
{
    LiftState *s = lift_state(m);
    uint32_t stub = callback_stub_addr(s->return_callback);
    uint16_t stub_off = (uint16_t)(stub - real_addr(CALLBACK_SEG, 0));
    cpu_push16(m, CALLBACK_SEG);
    cpu_push16(m, stub_off);
    uint16_t sp_after_return = (uint16_t)(m->cpu.r[R_SP].w + 4);
    cpu_set_seg(&m->cpu, S_CS, seg);
    m->cpu.eip = off;
    lift_far_call_landed(m);
    lift_coverage_hit(m, LIFT_COVERAGE_CALL);
    bool direct = call_direct(m, s);
    bool back = m->cpu.seg[S_CS] == CALLBACK_SEG && (uint16_t)m->cpu.eip == stub_off;
    if (direct && !back)
        s->direct_fallbacks++;
    if (!direct || !back)
        lift_run_nested(m, UINT32_MAX, sp_after_return, false); /* the sentinel callback ends it */
}

void lift_int_boundary(Machine *m)
{
    native_poll(m);
}

void guest_call_far(Machine *m, uint16_t seg, uint16_t off)
{
    uint16_t saved_cs = m->cpu.seg[S_CS], saved_ip = (uint16_t)m->cpu.eip;
    lift_dispatch_far_at(m, seg, off);
    cpu_set_seg(&m->cpu, S_CS, saved_cs);
    m->cpu.eip = saved_ip;
}

void lift_idle(Machine *m)
{
    static const uint8_t IDLE[] = { 0x9C, 0xFB, 0xF4, 0x9D, 0xCB };
    LiftState *s = lift_state(m);
    if (m->native) {
        native_idle(m);
        return;
    }
    if (!s->idle_off)
        s->idle_off = bios_rom_emit(m, IDLE, sizeof IDLE);
    guest_call_far(m, BIOS_SEG, s->idle_off);
}

/* The return address pushed is the lifted function's own entry, which is already hooked. */
void guest_call_near(Machine *m, uint16_t off)
{
    uint16_t entry_ip = (uint16_t)m->cpu.eip;
    uint32_t entry_linear = m->cpu.seg_base[S_CS] + entry_ip;
    cpu_push16(m, entry_ip);
    uint16_t sp_after_return = (uint16_t)(m->cpu.r[R_SP].w + 2);
    m->cpu.eip = off;
    lift_coverage_hit(m, LIFT_COVERAGE_CALL);
    LiftState *s = lift_state(m);
    bool direct = call_direct(m, s);
    bool back = m->cpu.seg_base[S_CS] + (m->cpu.eip & 0xFFFF) == entry_linear && m->cpu.r[R_SP].w == sp_after_return;
    if (direct && !back)
        s->direct_fallbacks++;
    if (!direct || !back) /* a call to the caller's own address looks returned before it ran */
        lift_run_nested(m, entry_linear, sp_after_return, false);
    m->cpu.eip = entry_ip;
}

/* Current address of a table entry in a loaded module; overlays have none that is fixed. */
static uint16_t entry_segment(Machine *m, const LiftedEntry *e)
{
    if (strchr(e->module, '#'))
        fatal("lift: %s %s is an overlay entry; it has no fixed address", e->module, e->name);
    uint16_t base = lift_module_base(m, e->module);
    if (!base)
        fatal("lift: %s %s called while %s is not loaded", e->module, e->name, e->module);
    return (uint16_t)(base + e->seg);
}

void lift_call_entry_far(Machine *m, const LiftedEntry *e)
{
    guest_call_far(m, entry_segment(m, e), e->off);
}

void lift_call_entry_near(Machine *m, const LiftedEntry *e)
{
    uint16_t seg = entry_segment(m, e);
    if (m->cpu.seg[S_CS] != seg)
        fatal("lift: near call to %s %s from CS=%04X, its segment is %04X", e->module, e->name,
              m->cpu.seg[S_CS], seg);
    guest_call_near(m, e->off);
}
