/* Announces code modules that programs load with plain file reads (LSCI drivers and DLLs), so
 * lifted functions in them get armed. Formats: TSN-INN-Decomp docs/lsci/interpreter.md 3.5. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
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

/* File offset where the code image starts: drivers are raw images, DLLs follow their relocations. */
static bool image_start(const DosFile *f, off_t *start)
{
    if (has_extension(f->dos_path, ".DRV")) {
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

void dos_note_module_read(Machine *m, const DosFile *f, off_t file_pos, uint16_t seg, uint16_t off,
                          uint16_t bytes)
{
    off_t start;
    if (!image_start(f, &start) || start < file_pos || start >= file_pos + bytes)
        return;
    uint32_t image_linear = real_addr(seg, off) + (uint32_t)(start - file_pos);
    if (image_linear % PARAGRAPH)
        return;
    off_t size = lseek(f->fd, 0, SEEK_END);
    lseek(f->fd, file_pos + bytes, SEEK_SET);
    const char *base = strrchr(f->dos_path, '\\');
    char name[16];
    snprintf(name, sizeof name, "%.15s", base ? base + 1 : f->dos_path);
    for (char *p = name; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    lift_module_loaded(m, name, (uint16_t)(image_linear / PARAGRAPH), (uint32_t)(size - start));
}
