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

typedef struct DosExecImage {
    uint16_t psp;            /* the child's PSP; its image starts at psp + 10h */
    uint16_t cs, ip, ss, sp; /* entry and stack as DOS hands them over */
    uint16_t ax;             /* FCB drive status, as AL/AH at program start */
} DosExecImage;

/* AH=4Bh AL=0 without the CPU: loads dos_path as a child of the current process (environment copied
 * from env_seg, 0 = the parent's; tail = length byte, text, CR; FCBs parsed from the tail), makes it the
 * current process and records the EXEC frame so terminate returns to the parent. CPU registers are
 * untouched. Returns 0 or a DOS error code (2 file not found, 8 no memory, 11 bad format). */
int dos_exec_load(Machine *m, const char *dos_path, const uint8_t *tail, uint16_t env_seg,
                  DosExecImage *out);
/* Sets registers, segments, SS:SP and CS:IP as DOS does when it starts a program. */
void dos_exec_enter(Machine *m, const DosExecImage *img);
/* The last child's return code as AH=4Dh returns it (AL code, AH termination type). */
uint16_t dos_last_exit_status(Machine *m);
/* Runs after terminate freed the child and made the parent current, before control goes to the INT 22h
 * address; NULL removes it. */
void dos_set_terminate_hook(Machine *m, void (*hook)(Machine *m, uint8_t code));

/* Sets the current directory of the current drive, e.g. "\\" or "\\INN". */
void dos_set_cwd(Machine *m, const char *dos_dir);

/* Leaves only the first kb KB of conventional memory to programs, as a machine with less memory
 * or more resident software would; the rest becomes a system block. */
void dos_limit_memory(Machine *m, unsigned kb);

/* Frees host resources (open files). Call before machine_destroy. */
void dos_shutdown(Machine *m);

/* Host-side DOS state (open files and their positions) for shadow verification. File contents
 * written in both shadow runs are not restored. */
size_t dos_snapshot_size(void);
void dos_snapshot_save(Machine *m, void *dst);
void dos_snapshot_load(Machine *m, const void *src);

/* Host file changes between begin and end are journaled: deletions, renames, creations and growth
 * of files (not bytes rewritten in place). Undo reverts those so far, between the two shadow runs;
 * end keeps the rest. */
void dos_journal_begin(Machine *m);
void dos_journal_undo(Machine *m);
void dos_journal_end(Machine *m);

#endif
