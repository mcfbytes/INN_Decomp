/* DOS path names: canonicalisation to "C:\DIR\NAME.EXT", case-insensitive mapping onto the host
 * directory that backs C:, and the directory/drive INT 21h functions. */
#define _POSIX_C_SOURCE 200809L /* fds, directories, localtime_r on any libc */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dos_int.h"

enum { MAX_COMPONENTS = 32, COMPONENT_MAX = 13, NAME_LEN = 8, EXT_LEN = 3 };

typedef char Component[COMPONENT_MAX];

static const struct { const char *name; DosDevice dev; } device_names[] = {
    {"CON", DEV_CON},   {"NUL", DEV_NUL},   {"AUX", DEV_AUX},   {"PRN", DEV_PRN},
    {"CLOCK$", DEV_CLOCK}, {"COM1", DEV_AUX}, {"COM2", DEV_AUX}, {"LPT1", DEV_PRN},
    {"LPT2", DEV_PRN},  {"LPT3", DEV_PRN},
};

void dos_read_name(Machine *m, uint16_t seg, uint16_t off, char *dst)
{
    mem_read_cstr(m, seg, off, dst, DOS_PATH_MAX);
}

/* Upper-cases and truncates one component to 8.3, as DOS does silently. */
static void normalize_component(const char *src, size_t len, Component out)
{
    size_t o = 0, i = 0, n = 0;
    for (; i < len && src[i] != '.' && n < NAME_LEN; i++, n++)
        out[o++] = (char)toupper((unsigned char)src[i]);
    while (i < len && src[i] != '.')
        i++;
    if (i < len && src[i] == '.' && i + 1 < len) {
        out[o++] = '.';
        for (i++, n = 0; i < len && src[i] != '.' && n < EXT_LEN; i++, n++)
            out[o++] = (char)toupper((unsigned char)src[i]);
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = 0;
}

/* Appends the components of path to comps, applying "." and ".."; returns the new count. */
static int split_path(const char *path, Component *comps, int count)
{
    while (*path) {
        while (*path == '\\' || *path == '/')
            path++;
        size_t len = strcspn(path, "\\/");
        if (!len)
            break;
        if (len == 1 && path[0] == '.') {
        } else if (len == 2 && path[0] == '.' && path[1] == '.') {
            if (count > 0)
                count--;
        } else if (count < MAX_COMPONENTS) {
            normalize_component(path, len, comps[count++]);
        }
        path += len;
    }
    return count;
}

static DosDevice device_of(const char *comp)
{
    char base[COMPONENT_MAX];
    size_t len = strcspn(comp, ".");
    memcpy(base, comp, len);
    base[len] = 0;
    for (size_t i = 0; i < sizeof device_names / sizeof device_names[0]; i++)
        if (!strcmp(base, device_names[i].name))
            return device_names[i].dev;
    return DEV_NONE;
}

/* Finds comp in host directory dir ignoring case; appends the real name to dir on success. */
static bool host_lookup(char *dir, size_t cap, const char *comp)
{
    size_t base = strlen(dir);
    snprintf(dir + base, cap - base, "/%s", comp);
    struct stat st;
    if (stat(dir, &st) == 0)
        return true;
    dir[base] = 0;
    DIR *dh = opendir(dir);
    if (!dh)
        return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(dh)))
        if (!strcasecmp(e->d_name, comp)) {
            snprintf(dir + base, cap - base, "/%s", e->d_name);
            found = true;
        }
    closedir(dh);
    return found;
}

static int map_to_host(DosState *d, Component *comps, int count, DosPath *out)
{
    snprintf(out->host, sizeof out->host, "%s", d->host_root);
    for (int i = 0; i < count; i++) {
        bool last = i == count - 1;
        if (!host_lookup(out->host, sizeof out->host, comps[i])) {
            if (!last)
                return DE_PATH_NOT_FOUND;
            size_t n = strlen(out->host);
            snprintf(out->host + n, sizeof out->host - n, "/%s", comps[i]);
            return DE_OK;
        }
        struct stat st;
        bool is_dir = stat(out->host, &st) == 0 && S_ISDIR(st.st_mode);
        if (!last && !is_dir)
            return DE_PATH_NOT_FOUND;
        if (last) {
            out->exists = true;
            out->is_dir = is_dir;
        }
    }
    if (count == 0)
        out->exists = out->is_dir = true;
    return DE_OK;
}

int dos_resolve(Machine *m, const char *name, DosPath *out)
{
    DosState *d = dos_state(m);
    memset(out, 0, sizeof *out);
    int drive = d->cur_drive;
    if (name[0] && name[1] == ':') {
        drive = toupper((unsigned char)name[0]) - 'A';
        name += 2;
    }
    if (drive != DOS_DRIVE_C)
        return DE_PATH_NOT_FOUND;
    Component comps[MAX_COMPONENTS];
    int count = 0;
    if (name[0] != '\\' && name[0] != '/')
        count = split_path(d->cwd, comps, 0);
    count = split_path(name, comps, count);
    strcpy(out->dos, "C:");
    for (int i = 0; i < count; i++) {
        size_t n = strlen(out->dos);
        snprintf(out->dos + n, sizeof out->dos - n, "\\%s", comps[i]);
    }
    if (count == 0)
        strcat(out->dos, "\\");
    if (count > 0 && (out->dev = device_of(comps[count - 1])) != DEV_NONE)
        return DE_OK;
    return map_to_host(d, comps, count, out);
}

/* "NAME.EXT" (or a pattern) to the blank-padded 11-byte FCB form; '*' fills with '?'. */
bool dos_name_to_fcb(const char *name, char fcb[11])
{
    memset(fcb, ' ', 11);
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        memcpy(fcb, name, strlen(name));
        return true;
    }
    int pos = 0, limit = NAME_LEN;
    for (const char *p = name; *p; p++) {
        if (*p == '.') {
            if (pos > NAME_LEN)
                return false;
            pos = NAME_LEN;
            limit = NAME_LEN + EXT_LEN;
        } else if (*p == '*') {
            while (pos < limit)
                fcb[pos++] = '?';
        } else if (pos < limit) {
            fcb[pos++] = (char)toupper((unsigned char)*p);
        } else {
            return false; /* longer than 8.3: not a DOS name */
        }
    }
    return true;
}

bool dos_fcb_match(const char pattern[11], const char name[11])
{
    for (int i = 0; i < 11; i++)
        if (pattern[i] != '?' && pattern[i] != name[i])
            return false;
    return true;
}

/* ---- current directory structure (read by some programs through SYSVARS) ---- */

enum { CDS_FLAGS = 0x43, CDS_ROOT_LEN = 0x4F, CDS_PHYSICAL = 0x4000, DOS_DIR_MAX = 64 };

/* Makes the directory part of a canonical "C:\..." path current; false if it is too long. */
static bool set_cwd(Machine *m, const char *canonical)
{
    const char *dir = canonical + 2;
    size_t len = strlen(dir);
    if (len >= DOS_DIR_MAX)
        return false;
    memcpy(dos_state(m)->cwd, dir, len + 1);
    dos_update_cds(m);
    return true;
}

void dos_update_cds(Machine *m)
{
    DosState *d = dos_state(m);
    for (int drive = 0; drive < DOS_LASTDRIVE; drive++) {
        uint32_t e = real_addr(DOS_DATA_SEG, (uint16_t)(DD_CDS + drive * CDS_ENTRY_SIZE));
        for (int i = 0; i < CDS_ENTRY_SIZE; i++)
            mem_wr8(m, e + (uint32_t)i, 0);
        const char *dir = drive == DOS_DRIVE_C ? d->cwd : "\\";
        mem_wr8(m, e, (uint8_t)('A' + drive));
        mem_wr8(m, e + 1, ':');
        mem_write_block(m, e + 2, dir, strlen(dir) + 1);
        mem_wr16(m, e + CDS_FLAGS, drive == DOS_DRIVE_C ? CDS_PHYSICAL : 0);
        mem_wr16(m, e + CDS_ROOT_LEN, 2);
    }
}

/* ---- INT 21h directory and drive functions ---- */

CallbackResult dos_select_drive(Machine *m)
{
    if (DL == DOS_DRIVE_C)
        dos_state(m)->cur_drive = DL;
    AL = DOS_LASTDRIVE;
    return CB_DONE;
}

CallbackResult dos_get_drive(Machine *m)
{
    AL = dos_state(m)->cur_drive;
    return CB_DONE;
}

static int resolve_arg(Machine *m, uint16_t seg, uint16_t off, DosPath *p)
{
    char name[DOS_PATH_MAX];
    dos_read_name(m, seg, off, name);
    return dos_resolve(m, name, p);
}

CallbackResult dos_mkdir(Machine *m)
{
    DosPath p;
    int err = resolve_arg(m, DS, DX, &p);
    if (err)
        return dos_fail(m, (uint16_t)err);
    if (p.exists || p.dev)
        return dos_fail(m, DE_ACCESS_DENIED);
    if (mkdir(p.host, 0777) != 0)
        return dos_fail(m, errno == ENOENT ? DE_PATH_NOT_FOUND : DE_ACCESS_DENIED);
    return dos_ok(m);
}

CallbackResult dos_rmdir(Machine *m)
{
    DosPath p;
    int err = resolve_arg(m, DS, DX, &p);
    if (err)
        return dos_fail(m, (uint16_t)err);
    if (!p.exists || !p.is_dir)
        return dos_fail(m, DE_PATH_NOT_FOUND);
    if (!strcmp(p.dos + 2, dos_state(m)->cwd))
        return dos_fail(m, DE_CURRENT_DIR);
    if (rmdir(p.host) != 0)
        return dos_fail(m, DE_ACCESS_DENIED);
    return dos_ok(m);
}

CallbackResult dos_chdir(Machine *m)
{
    DosPath p;
    int err = resolve_arg(m, DS, DX, &p);
    if (err || !p.exists || !p.is_dir || !set_cwd(m, p.dos))
        return dos_fail(m, DE_PATH_NOT_FOUND);
    return dos_ok(m);
}

void dos_set_cwd(Machine *m, const char *dos_dir)
{
    DosPath p;
    if (dos_resolve(m, dos_dir, &p) || !p.exists || !p.is_dir || !set_cwd(m, p.dos))
        log_msg("dos: set_cwd: %s is not a directory", dos_dir);
}

CallbackResult dos_getcwd(Machine *m)
{
    if (DL != 0 && DL != DOS_DRIVE_C + 1)
        return dos_fail(m, DE_INVALID_DRIVE);
    const char *cwd = dos_state(m)->cwd + 1; /* without the leading backslash */
    mem_write_block(m, real_addr(DS, SI), cwd, strlen(cwd) + 1);
    AX = 0x0100;
    return dos_ok(m);
}

CallbackResult dos_truename(Machine *m)
{
    DosPath p;
    int err = resolve_arg(m, DS, SI, &p);
    if (err)
        return dos_fail(m, (uint16_t)err);
    mem_write_block(m, real_addr(ES, DI), p.dos, strlen(p.dos) + 1);
    return dos_ok(m);
}

/* Fixed geometry: 32 KB clusters, so the numbers stay inside 16 bits like a 2 GB FAT16 disk. */
enum { SECTOR_BYTES = 512, CLUSTER_SECTORS = 64, TOTAL_CLUSTERS = 0xFFF0, FREE_CLUSTERS = 0x8000 };

static bool drive_ok(uint8_t dl) { return dl == 0 || dl == DOS_DRIVE_C + 1; }

CallbackResult dos_free_space(Machine *m)
{
    if (!drive_ok(DL)) {
        AX = 0xFFFF;
        return CB_DONE;
    }
    AX = CLUSTER_SECTORS;
    BX = FREE_CLUSTERS;
    CX = SECTOR_BYTES;
    DX = TOTAL_CLUSTERS;
    return CB_DONE;
}

static CallbackResult drive_data(Machine *m, uint8_t drive)
{
    if (!drive_ok(drive)) {
        AL = 0xFF;
        return CB_DONE;
    }
    AL = CLUSTER_SECTORS;
    CX = SECTOR_BYTES;
    DX = TOTAL_CLUSTERS;
    cpu_set_seg(&m->cpu, S_DS, DOS_DATA_SEG);
    BX = DD_MEDIA_ID;
    return CB_DONE;
}

CallbackResult dos_drive_data(Machine *m) { return drive_data(m, DL); }

CallbackResult dos_drive_data_default(Machine *m) { return drive_data(m, 0); }
