/* File handle functions on host files: create, open, read, write, seek, delete, rename,
 * attributes, timestamps, temporary files, commit and extended open. */
#define _POSIX_C_SOURCE 200809L /* fds, directories, localtime_r on any libc */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dos_int.h"

enum { ACCESS_MASK = 0x07, ACCESS_READ = 0, ACCESS_WRITE = 1, ACCESS_RW = 2 };
enum { IO_CHUNK = 0x10000 };

typedef enum OpenHow { OPEN_EXISTING, CREATE_TRUNC, CREATE_NEW } OpenHow;

static uint16_t errno_to_dos(int e)
{
    switch (e) {
    case ENOENT: return DE_FILE_NOT_FOUND;
    case ENOTDIR: return DE_PATH_NOT_FOUND;
    case EMFILE:
    case ENFILE: return DE_TOO_MANY_OPEN;
    case EEXIST: return DE_FILE_EXISTS;
    default: return DE_ACCESS_DENIED;
    }
}

static int host_flags(uint8_t mode, OpenHow how)
{
    int flags = O_RDWR;
    if (how == OPEN_EXISTING) {
        uint8_t access = mode & ACCESS_MASK;
        flags = access == ACCESS_READ ? O_RDONLY : access == ACCESS_WRITE ? O_WRONLY : O_RDWR;
    } else {
        flags |= O_CREAT | (how == CREATE_NEW ? O_EXCL : O_TRUNC);
    }
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    return flags;
}

/* Opens or creates name and gives it a handle in the current process. */
static int open_file(Machine *m, const char *name, OpenHow how, uint8_t mode, uint16_t *handle)
{
    DosPath p;
    int err = dos_resolve(m, name, &p);
    if (err)
        return err;
    if (!p.dev) {
        if (p.is_dir)
            return DE_ACCESS_DENIED;
        if (how == OPEN_EXISTING && !p.exists)
            return DE_FILE_NOT_FOUND;
        if (how == CREATE_NEW && p.exists)
            return DE_FILE_EXISTS;
    }
    if ((mode & ACCESS_MASK) > ACCESS_RW)
        return DE_ACCESS_CODE;
    int idx = dos_sft_alloc(m);
    if (idx < 0)
        return DE_TOO_MANY_OPEN;
    int fd = -1;
    if (!p.dev && (fd = open(p.host, host_flags(mode, how), 0666)) < 0)
        return errno_to_dos(errno);
    DosFile *f = &dos_state(m)->sft[idx];
    f->fd = fd;
    f->dev = p.dev;
    f->mode = how == OPEN_EXISTING ? mode : ACCESS_RW;
    snprintf(f->dos_path, sizeof f->dos_path, "%s", p.dos);
    int h = dos_alloc_handle(m, idx);
    if (h < 0) {
        if (fd >= 0)
            close(fd);
        f->fd = -1;
        return DE_TOO_MANY_OPEN;
    }
    *handle = (uint16_t)h;
    if (m->trace)
        log_msg("dos: handle %u = %s (sft %d)", h, p.dos, idx);
    return DE_OK;
}

static CallbackResult open_result(Machine *m, int err, uint16_t handle)
{
    if (err)
        return dos_fail(m, (uint16_t)err);
    AX = handle;
    return dos_ok(m);
}

static CallbackResult open_named(Machine *m, uint16_t seg, uint16_t off, OpenHow how, uint8_t mode)
{
    char name[DOS_PATH_MAX];
    dos_read_name(m, seg, off, name);
    uint16_t handle = 0;
    int err = open_file(m, name, how, mode, &handle);
    return open_result(m, err, handle);
}

CallbackResult dos_create(Machine *m) { return open_named(m, DS, DX, CREATE_TRUNC, ACCESS_RW); }

CallbackResult dos_open(Machine *m) { return open_named(m, DS, DX, OPEN_EXISTING, AL); }

CallbackResult dos_create_new(Machine *m) { return open_named(m, DS, DX, CREATE_NEW, ACCESS_RW); }

/* AH=5Ah: DS:DX is a directory ending in '\'; DOS appends a unique name to it. */
CallbackResult dos_create_temp(Machine *m)
{
    char dir[DOS_PATH_MAX], name[DOS_PATH_MAX + 16];
    dos_read_name(m, DS, DX, dir);
    size_t len = strlen(dir);
    const char *sep = len && dir[len - 1] != '\\' && dir[len - 1] != '/' && dir[len - 1] != ':' ? "\\" : "";
    for (unsigned n = (unsigned)(dos_state(m)->base_time & 0xFFFF); ; n++) {
        snprintf(name, sizeof name, "%s%sTMP%05X", dir, sep, n & 0xFFFFF);
        uint16_t handle = 0;
        int err = open_file(m, name, CREATE_NEW, ACCESS_RW, &handle);
        if (err == DE_FILE_EXISTS)
            continue;
        if (!err)
            mem_write_block(m, real_addr(DS, DX), name, strlen(name) + 1);
        return open_result(m, err, handle);
    }
}

/* AH=6Ch: BL access mode, DX action (low nibble: file exists, high nibble: file missing). */
enum { EXT_EXISTS_MASK = 0x0F, EXT_OPEN = 1, EXT_REPLACE = 2, EXT_CREATE = 0x10 };
enum { EXT_OPENED = 1, EXT_CREATED = 2, EXT_REPLACED = 3 };

CallbackResult dos_ext_open(Machine *m)
{
    char name[DOS_PATH_MAX];
    dos_read_name(m, DS, SI, name);
    DosPath p;
    int err = dos_resolve(m, name, &p);
    if (err)
        return dos_fail(m, (uint16_t)err);
    uint16_t handle = 0, action;
    bool exists = p.exists || p.dev;
    if (exists && (DL & EXT_EXISTS_MASK) == EXT_OPEN) {
        err = open_file(m, name, OPEN_EXISTING, BL, &handle);
        action = EXT_OPENED;
    } else if (exists && (DL & EXT_EXISTS_MASK) == EXT_REPLACE) {
        err = open_file(m, name, CREATE_TRUNC, BL, &handle);
        action = EXT_REPLACED;
    } else if (!exists && (DL & EXT_CREATE)) {
        err = open_file(m, name, CREATE_NEW, BL, &handle);
        action = EXT_CREATED;
    } else {
        return dos_fail(m, exists ? DE_FILE_EXISTS : DE_FILE_NOT_FOUND);
    }
    if (!err)
        CX = action;
    return open_result(m, err, handle);
}

CallbackResult dos_read(Machine *m)
{
    DosFile *f = dos_handle_file(m, BX);
    if (!f)
        return dos_fail(m, DE_INVALID_HANDLE);
    if ((f->mode & ACCESS_MASK) == ACCESS_WRITE)
        return dos_fail(m, DE_ACCESS_DENIED);
    uint8_t *buf = malloc(IO_CHUNK);
    if (!buf)
        fatal("out of memory");
    uint16_t got = 0;
    off_t file_pos = -1;
    if (f->dev == DEV_CON) {
        if (!dos_con_read(m, buf, CX, &got)) {
            free(buf);
            return CB_RETRY;
        }
    } else if (f->dev == DEV_NONE) {
        file_pos = lseek(f->fd, 0, SEEK_CUR);
        ssize_t n = read(f->fd, buf, CX);
        if (n < 0) {
            free(buf);
            return dos_fail(m, DE_ACCESS_DENIED);
        }
        got = (uint16_t)n;
    }
    mem_write_block(m, real_addr(DS, DX), buf, got);
    free(buf);
    if (file_pos >= 0)
        dos_note_module_read(m, f, file_pos, DS, DX, got);
    AX = got;
    return dos_ok(m);
}

CallbackResult dos_write(Machine *m)
{
    DosFile *f = dos_handle_file(m, BX);
    if (!f)
        return dos_fail(m, DE_INVALID_HANDLE);
    if ((f->mode & ACCESS_MASK) == ACCESS_READ)
        return dos_fail(m, DE_ACCESS_DENIED);
    uint8_t *buf = malloc(IO_CHUNK);
    if (!buf)
        fatal("out of memory");
    mem_read_block(m, real_addr(DS, DX), buf, CX);
    ssize_t n = CX;
    if (f->dev == DEV_CON) {
        dos_con_write(m, buf, CX);
    } else if (f->dev == DEV_NONE && CX == 0) {
        n = ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0 ? 0 : -1;
    } else if (f->dev == DEV_NONE) {
        n = write(f->fd, buf, CX);
    }
    free(buf);
    if (n < 0)
        return dos_fail(m, DE_ACCESS_DENIED);
    AX = (uint16_t)n;
    return dos_ok(m);
}

CallbackResult dos_lseek(Machine *m)
{
    DosFile *f = dos_handle_file(m, BX);
    if (!f)
        return dos_fail(m, DE_INVALID_HANDLE);
    if (AL > SEEK_END)
        return dos_fail(m, DE_FUNCTION);
    uint32_t raw = (uint32_t)CX << 16 | DX;
    off_t off = AL == SEEK_SET ? (off_t)raw : (off_t)(int32_t)raw;
    off_t pos = 0;
    if (f->dev == DEV_NONE && (pos = lseek(f->fd, off, AL)) < 0)
        return dos_fail(m, DE_ACCESS_DENIED);
    AX = (uint16_t)pos;
    DX = (uint16_t)((uint32_t)pos >> 16);
    return dos_ok(m);
}

CallbackResult dos_delete(Machine *m)
{
    char name[DOS_PATH_MAX];
    dos_read_name(m, DS, DX, name);
    DosPath p;
    int err = dos_resolve(m, name, &p);
    if (err)
        return dos_fail(m, (uint16_t)err);
    if (!p.exists || p.dev)
        return dos_fail(m, DE_FILE_NOT_FOUND);
    if (p.is_dir || dos_host_unlink(p.host) != 0)
        return dos_fail(m, DE_ACCESS_DENIED);
    return dos_ok(m);
}

CallbackResult dos_rename(Machine *m)
{
    char from_name[DOS_PATH_MAX], to_name[DOS_PATH_MAX];
    dos_read_name(m, DS, DX, from_name);
    dos_read_name(m, ES, DI, to_name);
    DosPath from, to;
    int err = dos_resolve(m, from_name, &from);
    if (!err)
        err = dos_resolve(m, to_name, &to);
    if (err)
        return dos_fail(m, (uint16_t)err);
    if (!from.exists)
        return dos_fail(m, DE_FILE_NOT_FOUND);
    if (to.exists || from.dev || to.dev)
        return dos_fail(m, DE_ACCESS_DENIED);
    if (dos_host_rename(from.host, to.host) != 0)
        return dos_fail(m, errno_to_dos(errno));
    return dos_ok(m);
}

CallbackResult dos_attrib(Machine *m)
{
    char name[DOS_PATH_MAX];
    dos_read_name(m, DS, DX, name);
    DosPath p;
    int err = dos_resolve(m, name, &p);
    if (err)
        return dos_fail(m, (uint16_t)err);
    struct stat st;
    if (!p.exists || stat(p.host, &st) != 0)
        return dos_fail(m, DE_FILE_NOT_FOUND);
    if (AL == 0) {
        uint16_t attr = S_ISDIR(st.st_mode) ? ATTR_DIR : ATTR_ARCHIVE;
        if (!(st.st_mode & S_IWUSR))
            attr |= ATTR_RDONLY;
        CX = AX = attr;
        return dos_ok(m);
    }
    if (AL != 1)
        return dos_fail(m, DE_FUNCTION);
    mode_t mode = (CX & ATTR_RDONLY) ? (st.st_mode & ~(mode_t)0222) : (st.st_mode | S_IWUSR);
    chmod(p.host, mode);
    return dos_ok(m);
}

static time_t dos_to_unix_time(uint16_t date, uint16_t time)
{
    struct tm tm = {0};
    tm.tm_year = (date >> 9) + 80;
    tm.tm_mon = (date >> 5 & 0x0F) - 1;
    tm.tm_mday = date & 0x1F;
    tm.tm_hour = time >> 11;
    tm.tm_min = time >> 5 & 0x3F;
    tm.tm_sec = (time & 0x1F) * 2;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

CallbackResult dos_file_time(Machine *m)
{
    DosFile *f = dos_handle_file(m, BX);
    if (!f)
        return dos_fail(m, DE_INVALID_HANDLE);
    struct stat st;
    if (AL == 0) {
        time_t t = f->dev == DEV_NONE && fstat(f->fd, &st) == 0 ? st.st_mtime : time(NULL);
        dos_unix_to_dos_time(t, &DX, &CX);
        return dos_ok(m);
    }
    if (AL != 1)
        return dos_fail(m, DE_FUNCTION);
    if (f->dev == DEV_NONE) {
        struct timespec ts[2] = {{dos_to_unix_time(DX, CX), 0}, {dos_to_unix_time(DX, CX), 0}};
        futimens(f->fd, ts);
    }
    return dos_ok(m);
}

CallbackResult dos_commit(Machine *m)
{
    DosFile *f = dos_handle_file(m, BX);
    if (!f)
        return dos_fail(m, DE_INVALID_HANDLE);
    if (f->dev == DEV_NONE)
        fsync(f->fd);
    return dos_ok(m);
}
