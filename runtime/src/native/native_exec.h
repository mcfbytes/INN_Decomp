/* Native EXEC: loads a program through the DOS loader and runs it to termination without going
 * through INT 21h, as docs/native.md sections 9 and 11 describe. */
#ifndef DOSRT_NATIVE_EXEC_H
#define DOSRT_NATIVE_EXEC_H

#include <stdint.h>

#include "machine.h"

/* Runs a program as DOS EXEC function 0 would; returns its exit code (AH=4Dh), or -1 with *dos_error.
 * Restores the caller's registers and SS:SP bytes; 0 with m->exit_requested set if the frontend quit. */
int native_exec(Machine *m, const char *dos_path, const uint8_t *tail /* length byte, text, CR */,
                uint16_t env_seg, uint16_t *dos_error);

/* The program dos_start_program set up is run like a child of the shell; returns when it terminates,
 * with m->exit_requested set. */
void native_run_top(Machine *m);

/* The CPU runs from CS:IP, with interrupts held, until it reaches the native start of its program (a
 * packed program's unpacker ends there). False when it stops elsewhere first. */
bool native_unpack_to_start(Machine *m);

/* Mixed-mode device hand-off around a child that runs on the emulator (package 10b fills them in). */
void native_handoff_to_emulator(Machine *m);
void native_handoff_from_emulator(Machine *m);

/* INN_DUMP_AT=<point>:<n> and INN_DUMP_DIR write memory and DOS state at the n-th hit of exec, kernel
 * or start (docs/lifting.md). */
void native_dump_point(Machine *m, const char *point);

#endif
