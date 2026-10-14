/* DOS host-side state for shadow verification: the state struct plus every open file's position,
 * so a function that reads a file sees the same bytes in both shadow runs. */
#include <stdio.h>
#include <string.h>
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

typedef struct JournalEntry {
    bool deleted;              /* else renamed */
    char path[HOST_PATH_MAX];  /* deleted file, or rename source */
    char other[HOST_PATH_MAX]; /* where the deleted file is kept, or rename target */
} JournalEntry;

/* Shadow runs are nested in one machine's step, so one journal serves the process. */
static struct { bool active; int count, serial; JournalEntry e[JOURNAL_MAX]; } journal;

void dos_journal_begin(Machine *m)
{
    (void)m;
    journal.active = true;
    journal.count = 0;
}

void dos_journal_undo(Machine *m)
{
    (void)m;
    while (journal.count > 0) {
        JournalEntry *e = &journal.e[--journal.count];
        if (e->deleted) {
            unlink(e->path); /* recreated by the run being undone */
            rename(e->other, e->path);
        } else {
            rename(e->other, e->path);
        }
    }
}

void dos_journal_end(Machine *m)
{
    (void)m;
    for (int i = 0; i < journal.count; i++)
        if (journal.e[i].deleted)
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
    e->deleted = true;
    snprintf(e->path, sizeof e->path, "%s", path);
    journal.count++;
    return 0;
}

int dos_host_rename(const char *from, const char *to)
{
    if (rename(from, to) != 0)
        return -1;
    if (journal.active && journal.count < JOURNAL_MAX) {
        JournalEntry *e = &journal.e[journal.count++];
        e->deleted = false;
        snprintf(e->path, sizeof e->path, "%s", from);
        snprintf(e->other, sizeof e->other, "%s", to);
    }
    return 0;
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
