/* dosrt DOS layer: a high-level emulated MS-DOS 5.0 kernel (INT 20h-2Fh, INT 67h stub). Guest
 * drive C: is a host directory; memory is a real MCB chain in guest RAM so programs can walk it. */
#ifndef DOSRT_DOS_H
#define DOSRT_DOS_H

#include <stdbool.h>

#include <stddef.h>

#include "machine.h"

/* Registers the DOS interrupt vectors, builds kernel data and the root shell process. Call after
 * bios_init. c_drive_host_dir is the host directory that becomes C:\. */
void dos_init(Machine *m, const char *c_drive_host_dir);

/* Runs dos_path (.COM/.EXE probed if no extension) as COMMAND.COM would. When that program
 * terminates, m->exit_requested is set and m->exit_code holds its return code. */
bool dos_start_program(Machine *m, const char *dos_path, const char *args);

/* Sets the current directory of the current drive, e.g. "\\" or "\\INN". */
void dos_set_cwd(Machine *m, const char *dos_dir);

/* Frees host resources (open files). Call before machine_destroy. */
void dos_shutdown(Machine *m);

/* Host-side DOS state (open files and their positions) for shadow verification. File contents
 * written in both shadow runs are not restored. */
size_t dos_snapshot_size(void);
void dos_snapshot_save(Machine *m, void *dst);
void dos_snapshot_load(Machine *m, const void *src);

#endif
