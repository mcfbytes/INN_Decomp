/* DOS host-side state for shadow verification: the state struct plus every open file's position,
 * so a function that reads a file sees the same bytes in both shadow runs. */
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
