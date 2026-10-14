/* Directory search through the DTA (AH=1Ah/2Fh/4Eh/4Fh) and file name parsing into FCBs
 * (AH=29h, also used for the two FCBs in a new PSP). */
#define _POSIX_C_SOURCE 200809L /* fds, directories, localtime_r on any libc */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dos_int.h"

/* Our layout of the 21 reserved bytes at the start of the DTA. */
enum {
    DTA_DRIVE = 0x00, DTA_PATTERN = 0x01, DTA_SATTR = 0x0C, DTA_INDEX = 0x0D, DTA_SLOT = 0x0F,
    DTA_ATTR = 0x15, DTA_TIME = 0x16, DTA_DATE = 0x18, DTA_SIZE = 0x1A, DTA_NAME = 0x1E,
    DTA_NAME_LEN = 13,
};

void dos_unix_to_dos_time(time_t t, uint16_t *date, uint16_t *time)
{
    struct tm tm;
    localtime_r(&t, &tm);
    int year = tm.tm_year < 80 ? 0 : tm.tm_year - 80;
    *date = (uint16_t)(year << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    *time = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
}

CallbackResult dos_set_dta(Machine *m)
{
    dos_state(m)->dta_seg = DS;
    dos_state(m)->dta_off = DX;
    return CB_DONE;
}

CallbackResult dos_get_dta(Machine *m)
{
    cpu_set_seg(&m->cpu, S_ES, dos_state(m)->dta_seg);
    BX = dos_state(m)->dta_off;
    return CB_DONE;
}

typedef struct DirEntry {
    char name[DTA_NAME_LEN];      /* as DOS shows it */
    char host_name[DTA_NAME_LEN]; /* as stored on the host */
} DirEntry;

static int entry_cmp(const void *a, const void *b)
{
    return strcmp(((const DirEntry *)a)->name, ((const DirEntry *)b)->name);
}

/* The DOS-visible (8.3, upper-case) names in a host directory, sorted for a stable order. */
static DirEntry *list_dir(const char *host_dir, bool is_root, int *count)
{
    *count = 0;
    DIR *dh = opendir(host_dir);
    if (!dh)
        return NULL;
    int cap = 64;
    DirEntry *list = malloc(sizeof *list * (size_t)cap);
    struct dirent *e;
    char fcb[11];
    while (list && (e = readdir(dh))) {
        bool dots = !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..");
        if ((dots && is_root) || strlen(e->d_name) >= DTA_NAME_LEN || !dos_name_to_fcb(e->d_name, fcb))
            continue;
        if (*count == cap && !(list = realloc(list, sizeof *list * (size_t)(cap *= 2))))
            break;
        DirEntry *d = &list[(*count)++];
        memcpy(d->host_name, e->d_name, strlen(e->d_name) + 1);
        for (size_t i = 0; i <= strlen(e->d_name); i++)
            d->name[i] = (char)toupper((unsigned char)e->d_name[i]);
    }
    closedir(dh);
    if (!list)
        fatal("out of memory");
    qsort(list, (size_t)*count, sizeof *list, entry_cmp);
    return list;
}

static void fill_dta_entry(Machine *m, uint32_t dta, const char *name, const struct stat *st)
{
    uint16_t date, time;
    dos_unix_to_dos_time(st->st_mtime, &date, &time);
    uint8_t attr = S_ISDIR(st->st_mode) ? ATTR_DIR : ATTR_ARCHIVE;
    if (!(st->st_mode & S_IWUSR))
        attr |= ATTR_RDONLY;
    mem_wr8(m, dta + DTA_ATTR, attr);
    mem_wr16(m, dta + DTA_TIME, time);
    mem_wr16(m, dta + DTA_DATE, date);
    mem_wr32(m, dta + DTA_SIZE, S_ISDIR(st->st_mode) ? 0 : (uint32_t)st->st_size);
    char padded[DTA_NAME_LEN] = {0};
    snprintf(padded, sizeof padded, "%s", name);
    mem_write_block(m, dta + DTA_NAME, padded, DTA_NAME_LEN);
}

/* Returns the index-th entry of the slot's directory matching the DTA's pattern. */
static CallbackResult search(Machine *m, uint32_t dta, int no_match_error)
{
    DosState *d = dos_state(m);
    uint16_t slot = mem_rd16(m, dta + DTA_SLOT), index = mem_rd16(m, dta + DTA_INDEX);
    uint8_t sattr = mem_rd8(m, dta + DTA_SATTR);
    if (slot >= DOS_SEARCH_SLOTS || !d->search_dirs[slot][0])
        return dos_fail(m, DE_NO_MORE_FILES);
    char pattern[11], fcb[11], path[HOST_PATH_MAX + DTA_NAME_LEN + 1];
    mem_read_block(m, dta + DTA_PATTERN, pattern, sizeof pattern);
    const char *dir = d->search_dirs[slot];
    int count;
    DirEntry *list = list_dir(dir, !strcmp(dir, d->host_root), &count);
    for (int i = 0, seen = 0; i < count; i++) {
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, list[i].host_name);
        if (!dos_name_to_fcb(list[i].name, fcb) || !dos_fcb_match(pattern, fcb) || stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode) && !(sattr & ATTR_DIR))
            continue;
        if (seen++ < index)
            continue;
        fill_dta_entry(m, dta, list[i].name, &st);
        mem_wr16(m, dta + DTA_INDEX, (uint16_t)(index + 1));
        free(list);
        return dos_ok(m);
    }
    free(list);
    return dos_fail(m, (uint16_t)no_match_error);
}

CallbackResult dos_find_first(Machine *m)
{
    DosState *d = dos_state(m);
    char name[DOS_PATH_MAX];
    dos_read_name(m, DS, DX, name);
    DosPath p;
    if (dos_resolve(m, name, &p) || p.dev)
        return dos_fail(m, DE_PATH_NOT_FOUND);
    char *leaf_dos = strrchr(p.dos, '\\'), *leaf_host = strrchr(p.host, '/');
    char pattern[11];
    if (!leaf_dos || !leaf_host || !dos_name_to_fcb(leaf_dos + 1, pattern))
        return dos_fail(m, DE_FILE_NOT_FOUND);
    if ((CX & 0xFF) == ATTR_VOLUME)
        return dos_fail(m, DE_NO_MORE_FILES); /* the disk has no volume label */
    int slot = d->search_next;
    d->search_next = (d->search_next + 1) % DOS_SEARCH_SLOTS;
    size_t dir_len = (size_t)(leaf_host - p.host);
    snprintf(d->search_dirs[slot], HOST_PATH_MAX, "%.*s", (int)dir_len, p.host);
    uint32_t dta = real_addr(d->dta_seg, d->dta_off);
    mem_wr8(m, dta + DTA_DRIVE, DOS_DRIVE_C + 1);
    mem_write_block(m, dta + DTA_PATTERN, pattern, sizeof pattern);
    mem_wr8(m, dta + DTA_SATTR, (uint8_t)CX);
    mem_wr16(m, dta + DTA_INDEX, 0);
    mem_wr16(m, dta + DTA_SLOT, (uint16_t)slot);
    return search(m, dta, DE_NO_MORE_FILES);
}

CallbackResult dos_find_next(Machine *m)
{
    DosState *d = dos_state(m);
    return search(m, real_addr(d->dta_seg, d->dta_off), DE_NO_MORE_FILES);
}

/* ---- AH=29h: parse a file name into an FCB ---- */

enum { PARSE_SKIP_SEP = 1, PARSE_KEEP_DRIVE = 2, PARSE_KEEP_NAME = 4, PARSE_KEEP_EXT = 8 };
enum { FCB_NAME = 1, FCB_EXT = 9, FCB_PARSED = 12, PARSE_BUF = 128 };

static bool is_terminator(char c)
{
    return (unsigned char)c <= ' ' || strchr("./\\\"[]:|<>+=;,", c) != NULL;
}

/* Copies up to len characters of a name field; '*' pads with '?'. */
static const char *parse_field(const char *s, uint8_t *field, int len, bool *wild)
{
    memset(field, ' ', (size_t)len);
    int i = 0;
    for (; *s && !is_terminator(*s); s++) {
        if (*s == '*') {
            while (i < len)
                field[i++] = '?';
            *wild = true;
        } else if (i < len) {
            if (*s == '?')
                *wild = true;
            field[i++] = (uint8_t)toupper((unsigned char)*s);
        }
    }
    return s;
}

int dos_parse_name_fcb(const char *s, uint8_t flags, uint8_t fcb[12], uint8_t *result)
{
    const char *start = s;
    while (*s == ' ' || *s == '\t')
        s++;
    if ((flags & PARSE_SKIP_SEP) && *s && strchr(":;.,=+", *s))
        for (s++; *s == ' ' || *s == '\t';)
            s++;
    *result = 0;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') {
        int drive = toupper((unsigned char)s[0]) - 'A' + 1;
        fcb[0] = (uint8_t)drive;
        if (drive != DOS_DRIVE_C + 1)
            *result = 0xFF;
        s += 2;
    } else if (!(flags & PARSE_KEEP_DRIVE)) {
        fcb[0] = 0;
    }
    bool wild = false;
    uint8_t name[8], ext[3];
    const char *after = parse_field(s, name, 8, &wild);
    if (after != s || !(flags & PARSE_KEEP_NAME))
        memcpy(fcb + FCB_NAME, name, sizeof name);
    s = after;
    if (*s == '.') {
        s = parse_field(s + 1, ext, 3, &wild);
        memcpy(fcb + FCB_EXT, ext, sizeof ext);
    } else if (!(flags & PARSE_KEEP_EXT)) {
        memset(fcb + FCB_EXT, ' ', 3);
    }
    if (wild && *result == 0)
        *result = 1;
    return (int)(s - start);
}

CallbackResult dos_parse_fcb(Machine *m)
{
    char text[PARSE_BUF];
    mem_read_cstr(m, DS, SI, text, sizeof text);
    uint8_t fcb[FCB_PARSED];
    uint32_t dst = real_addr(ES, DI);
    mem_read_block(m, dst, fcb, sizeof fcb);
    uint8_t result;
    int used = dos_parse_name_fcb(text, AL, fcb, &result);
    mem_write_block(m, dst, fcb, sizeof fcb);
    SI = (uint16_t)(SI + used);
    AL = result;
    return CB_DONE;
}
