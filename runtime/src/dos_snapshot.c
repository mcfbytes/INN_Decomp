/* DOS host-side state for shadow verification: the state struct plus every open file's position,
 * so a function that reads a file sees the same bytes in both shadow runs. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dos_int.h"

typedef struct DosSnapshot {
    DosState state;
    off_t positions[DOS_SFT_MAX];
} DosSnapshot;

size_t dos_snapshot_size(void) { return sizeof(DosSnapshot); }

void dos_snapshot_save(Machine *m, void *dst)
{
    DosSnapshot *snap = dst;
    if (!m->dos) {
        memset(snap, 0, sizeof *snap);
        return;
    }
    snap->state = *dos_state(m);
    for (int i = 0; i < DOS_SFT_MAX; i++) {
        const DosFile *f = &snap->state.sft[i];
        snap->positions[i] = f->refs && f->fd >= 0 ? lseek(f->fd, 0, SEEK_CUR) : 0;
    }
}

static bool snapshot_uses_fd(const DosSnapshot *snap, int fd)
{
    for (int i = 0; i < DOS_SFT_MAX; i++)
        if (snap->state.sft[i].refs && snap->state.sft[i].fd == fd)
            return true;
    return false;
}

/* ---- journal of host deletions and renames ---- */

enum { JOURNAL_MAX = 32 };

typedef enum JournalKind { J_DELETED, J_RENAMED, J_CREATED, J_SIZED, J_SIZED_FD } JournalKind;

typedef struct JournalEntry {
    JournalKind kind;
    char path[HOST_PATH_MAX];  /* the file (J_DELETED: where it was, J_RENAMED: the source) */
    char other[HOST_PATH_MAX]; /* J_DELETED: where it is kept, J_RENAMED: the target */
    int fd;                    /* J_SIZED_FD: a descriptor open when the journal began */
    off_t size;                /* J_SIZED, J_SIZED_FD: size to truncate back to */
} JournalEntry;

/* Shadow runs are nested in one machine's step, so one journal serves the process. */
static struct {
    bool active;
    int count, serial;
    JournalEntry e[JOURNAL_MAX];
    int begin_fds[DOS_SFT_MAX], begin_count;     /* open when the journal began */
    int deferred_fds[DOS_SFT_MAX], deferred_count; /* of those, closed by a run */
} journal;

static JournalEntry *journal_add(JournalKind kind, const char *path)
{
    if (!journal.active || journal.count >= JOURNAL_MAX)
        return NULL;
    JournalEntry *e = &journal.e[journal.count++];
    e->kind = kind;
    snprintf(e->path, sizeof e->path, "%s", path ? path : "");
    return e;
}

/* Files open now may grow during the run; their sizes are restored on undo. */
void dos_journal_begin(Machine *m)
{
    journal.active = true;
    journal.count = journal.begin_count = journal.deferred_count = 0;
    if (!m || !m->dos)
        return;
    const DosState *d = dos_state(m);
    for (int i = 0; i < DOS_SFT_MAX; i++) {
        struct stat st;
        if (!d->sft[i].refs || d->sft[i].fd < 0 || fstat(d->sft[i].fd, &st) != 0)
            continue;
        journal.begin_fds[journal.begin_count++] = d->sft[i].fd;
        JournalEntry *e = journal_add(J_SIZED_FD, NULL);
        if (e) {
            e->fd = d->sft[i].fd;
            e->size = st.st_size;
        }
    }
}

void dos_journal_undo(Machine *m)
{
    (void)m;
    while (journal.count > 0) {
        JournalEntry *e = &journal.e[--journal.count];
        switch (e->kind) {
        case J_DELETED:
            unlink(e->path); /* recreated by the run being undone */
            rename(e->other, e->path);
            break;
        case J_RENAMED: rename(e->other, e->path); break;
        case J_CREATED: unlink(e->path); break;
        case J_SIZED: if (truncate(e->path, e->size) != 0) { /* gone again: nothing to restore */ } break;
        case J_SIZED_FD: if (ftruncate(e->fd, e->size) != 0) { /* closed by the run */ } break;
        }
    }
}

static bool fd_listed(const int *fds, int count, int fd)
{
    for (int i = 0; i < count; i++)
        if (fds[i] == fd)
            return true;
    return false;
}

/* A run must not free a descriptor the restored state still names: the next open would reuse its
 * number for another file. Closing waits for the end, when only the final state counts. */
void dos_host_close(int fd)
{
    if (!journal.active || !fd_listed(journal.begin_fds, journal.begin_count, fd)) {
        close(fd);
        return;
    }
    if (!fd_listed(journal.deferred_fds, journal.deferred_count, fd))
        journal.deferred_fds[journal.deferred_count++] = fd;
}

void dos_journal_end(Machine *m)
{
    for (int i = 0; i < journal.deferred_count; i++) {
        bool used = false;
        for (int k = 0; m && m->dos && k < DOS_SFT_MAX; k++)
            used |= dos_state(m)->sft[k].refs && dos_state(m)->sft[k].fd == journal.deferred_fds[i];
        if (!used)
            close(journal.deferred_fds[i]);
    }
    journal.deferred_count = journal.begin_count = 0;
    for (int i = 0; i < journal.count; i++)
        if (journal.e[i].kind == J_DELETED)
            unlink(journal.e[i].other);
    journal.active = false;
    journal.count = 0;
}

int dos_host_unlink(const char *path)
{
    if (!journal.active || journal.count >= JOURNAL_MAX)
        return unlink(path);
    JournalEntry *e = &journal.e[journal.count];
    snprintf(e->other, sizeof e->other, "%s.shadow%d", path, journal.serial++);
    if (rename(path, e->other) != 0)
        return -1;
    e->kind = J_DELETED;
    snprintf(e->path, sizeof e->path, "%s", path);
    journal.count++;
    return 0;
}

int dos_host_rename(const char *from, const char *to)
{
    if (rename(from, to) != 0)
        return -1;
    JournalEntry *e = journal_add(J_RENAMED, from);
    if (e)
        snprintf(e->other, sizeof e->other, "%s", to);
    return 0;
}

int dos_host_open(const char *path, int flags, bool exists)
{
    if (journal.active && exists && (flags & O_TRUNC) && dos_host_unlink(path) != 0)
        return -1; /* a truncating create keeps the old file aside, as a delete does */
    struct stat st;
    bool grows = journal.active && exists && !(flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY;
    if (grows && stat(path, &st) == 0) {
        JournalEntry *e = journal_add(J_SIZED, path);
        if (e)
            e->size = st.st_size;
    }
    int fd = open(path, flags, 0666);
    if (fd >= 0 && journal.active && (!exists || (flags & O_TRUNC)))
        journal_add(J_CREATED, path);
    return fd;
}

/* Descriptors opened after the snapshot are closed, or every shadow run would leak them. */
void dos_snapshot_load(Machine *m, const void *src)
{
    const DosSnapshot *snap = src;
    if (!m->dos)
        return;
    DosState *d = dos_state(m);
    for (int i = 0; i < DOS_SFT_MAX; i++)
        if (d->sft[i].refs && d->sft[i].fd >= 0 && !snapshot_uses_fd(snap, d->sft[i].fd))
            close(d->sft[i].fd);
    *d = snap->state;
    for (int i = 0; i < DOS_SFT_MAX; i++)
        if (d->sft[i].refs && d->sft[i].fd >= 0)
            lseek(d->sft[i].fd, snap->positions[i], SEEK_SET);
}
