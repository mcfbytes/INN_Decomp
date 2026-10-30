/* Announces code that programs load with plain file reads (LSCI drivers and DLLs, Miles .ADV drivers: TSN-INN-Decomp
 * docs/lsci/interpreter.md 3.5; Borland overlays: docs/lifting.md), so its lifted functions get armed. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dos_int.h"
#include "lift.h"

enum { DLL_HEADER_WORDS = 2, PARAGRAPH = 16 };

static bool has_extension(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    if (n < e)
        return false;
    for (size_t i = 0; i < e; i++)
        if (toupper((unsigned char)path[n - e + i]) != ext[i])
            return false;
    return true;
}

/* File offset where the code image starts: drivers (.DRV, Miles .ADV) are raw images, DLLs follow their relocations. */
static bool image_start(const DosFile *f, off_t *start)
{
    if (has_extension(f->dos_path, ".DRV") || has_extension(f->dos_path, ".ADV")) {
        *start = 0;
        return true;
    }
    if (!has_extension(f->dos_path, ".DLL"))
        return false;
    uint8_t header[DLL_HEADER_WORDS * 2];
    if (pread(f->fd, header, sizeof header, 0) != (ssize_t)sizeof header)
        return false;
    uint16_t relocations = (uint16_t)(header[2] | header[3] << 8);
    *start = (off_t)sizeof header + 2 * (off_t)relocations;
    return true;
}

/* True when the path contains this directory component, case-insensitively. */
static bool has_component(const char *path, const char *component)
{
    size_t n = strlen(component);
    for (const char *p = path; *p; p++) {
        if (*p != '\\' && *p != '/')
            continue;
        const char *q = p + 1;
        size_t i = 0;
        while (i < n && q[i] && toupper((unsigned char)q[i]) == component[i])
            i++;
        if (i == n && (q[i] == '\\' || q[i] == '/' || q[i] == 0))
            return true;
    }
    return false;
}

/* Upper-case file name of a DOS path, as loaders announce modules. The 1995 Cawdor client ships its
 * own TWGENN.EXE build under \INN\CAWDOR; it has different code from Twinion's gallery at the same
 * module name, so it is announced under its own name. */
static bool name_is(const char *name, const char *want)
{
    for (; *name && *want; name++, want++)
        if (toupper((unsigned char)*name) != *want)
            return false;
    return *name == 0 && *want == 0;
}

void lift_module_name(const char *dos_path, char *out, size_t cap)
{
    const char *base = strrchr(dos_path, '\\');
    base = base ? base + 1 : dos_path;
    if (has_component(dos_path, "CAWDOR") && name_is(base, "TWGENN.EXE")) {
        snprintf(out, cap, "TWGENN-CAW.EXE");
        return;
    }
    snprintf(out, cap, "%s", base);
    for (char *p = out; *p; p++)
        *p = (char)toupper((unsigned char)*p);
}

static void module_name(const char *dos_path, char *out, size_t cap)
{
    lift_module_name(dos_path, out, cap);
}

/* Overlay list of the EXE behind f, parsed once per file. */
static const DosOverlayCache *overlays_of(Machine *m, const DosFile *f)
{
    DosOverlayCache *c = &dos_state(m)->overlays;
    if (!strcmp(c->dos_path, f->dos_path))
        return c;
    snprintf(c->dos_path, sizeof c->dos_path, "%s", f->dos_path);
    c->count = 0;
    struct stat st;
    uint8_t *exe = fstat(f->fd, &st) == 0 && st.st_size > 0 ? malloc((size_t)st.st_size) : NULL;
    if (exe && pread(f->fd, exe, (size_t)st.st_size, 0) == st.st_size) {
        int n = borland_overlays_parse(exe, (size_t)st.st_size, c->list, DOS_OVERLAYS_MAX);
        c->count = n > 0 ? n : 0;
    }
    free(exe);
    return c;
}

/* A running program read overlay code from its own EXE: announce each overlay the read holds whole. */
static void note_overlay_read(Machine *m, const DosFile *f, off_t file_pos, uint16_t seg, uint16_t off,
                              uint16_t bytes)
{
    if (!has_extension(f->dos_path, ".EXE"))
        return;
    char name[16];
    module_name(f->dos_path, name, sizeof name);
    uint16_t program_seg = lift_module_base(m, name);
    if (!program_seg)
        return;
    const DosOverlayCache *c = overlays_of(m, f);
    for (int i = 0; i < c->count; i++) {
        const BorlandOverlay *o = &c->list[i];
        if (o->file_off < file_pos || o->file_off + o->code_bytes > file_pos + bytes)
            continue;
        uint32_t linear = real_addr(seg, off) + (uint32_t)(o->file_off - file_pos);
        if (linear % PARAGRAPH)
            continue;
        char overlay[32];
        snprintf(overlay, sizeof overlay, "%s#%04X", name, o->stub_seg);
        /* The fixups follow the code only until the manager applied them; the next overlay may
         * be read over them. */
        lift_overlay_loaded(m, overlay, (uint16_t)(linear / PARAGRAPH), o->code_bytes, program_seg);
    }
}

void dos_note_module_read(Machine *m, const DosFile *f, off_t file_pos, uint16_t seg, uint16_t off,
                          uint16_t bytes)
{
    off_t start;
    if (!image_start(f, &start)) {
        note_overlay_read(m, f, file_pos, seg, off, bytes);
        return;
    }
    if (start < file_pos || start >= file_pos + bytes)
        return;
    uint32_t image_linear = real_addr(seg, off) + (uint32_t)(start - file_pos);
    if (image_linear % PARAGRAPH)
        return;
    off_t size = lseek(f->fd, 0, SEEK_END);
    lseek(f->fd, file_pos + bytes, SEEK_SET);
    char name[16];
    module_name(f->dos_path, name, sizeof name);
    uint32_t image_bytes = size > start ? (uint32_t)(size - start) : 0;
    uint8_t *image = image_bytes ? malloc(image_bytes) : NULL;
    if (image && pread(f->fd, image, image_bytes, start) == (ssize_t)image_bytes)
        lift_module_loaded_image(m, name, (uint16_t)(image_linear / PARAGRAPH), image, image_bytes);
    else
        log_msg("lift: %s at %04X: cannot read its image; running its original code",
                name, (uint16_t)(image_linear / PARAGRAPH));
    free(image);
}
