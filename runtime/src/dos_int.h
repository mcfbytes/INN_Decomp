/* Internal interface shared by the dos*.c files. Not for use outside the DOS layer. */
#ifndef DOSRT_DOS_INT_H
#define DOSRT_DOS_INT_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <sys/types.h>

#include "borland_overlay.h"
#include "dos.h"
#include "machine.h"

/* ---- memory layout and limits ---- */

enum {
    DOS_DATA_SEG = 0x0060,  /* kernel data: SYSVARS, SFT header, InDOS, tables */
    DOS_FIRST_MCB = 0x0100, /* arena start */
    DOS_ARENA_END = 0xA000, /* first paragraph past conventional memory */
    PARA = 16,
};

/* Offsets inside DOS_DATA_SEG. The List of Lists has the first-MCB word just before it. */
enum {
    DD_LOL = 0x0026,
    DD_FIRST_MCB = DD_LOL - 2,
    DD_NUL_DEV = DD_LOL + 0x22,
    DD_CRIT_ERR = 0x00FF,
    DD_INDOS = 0x0100,
    DD_SFT = 0x0110,
    DD_MEDIA_ID = 0x0120,
    DD_DBCS = 0x0130,
    DD_UPCASE = 0x0140,   /* word 0x80 + 128 bytes */
    DD_FILECHARS = 0x01D0, /* filename terminator table */
    DD_COLLATE = 0x0200,  /* word 0x100 + 256 bytes */
    DD_CDS = 0x0310,
    DD_DPB = 0x04D0,
};

enum {
    DOS_PATH_MAX = 128,
    HOST_PATH_MAX = 1024,
    DOS_SFT_MAX = 64,
    DOS_JFT_DEFAULT = 20,
    DOS_EXEC_DEPTH = 16,
    DOS_SEARCH_SLOTS = 32,
    DOS_LASTDRIVE = 5, /* A..E exist in the CDS; only C: is backed */
    DOS_DRIVE_C = 2,
    CDS_ENTRY_SIZE = 0x58,
    JFT_CLOSED = 0xFF,
};

/* PSP field offsets. */
enum {
    PSP_INT20 = 0x00, PSP_TOP = 0x02, PSP_CALL5 = 0x05, PSP_INT22 = 0x0A, PSP_INT23 = 0x0E,
    PSP_INT24 = 0x12, PSP_PARENT = 0x16, PSP_JFT = 0x18, PSP_ENV = 0x2C, PSP_SSSP = 0x2E,
    PSP_JFT_SIZE = 0x32, PSP_JFT_PTR = 0x34, PSP_PREV = 0x38, PSP_VERSION = 0x40,
    PSP_DISPATCH = 0x50, PSP_FCB1 = 0x5C, PSP_FCB2 = 0x6C, PSP_TAIL = 0x80,
    PSP_PARAS = 0x10,
};

/* DOS error codes (INT 21h AX on carry). */
/* Memory control block fields, relative to the MCB paragraph. */
enum { MCB_TYPE = 0, MCB_OWNER = 1, MCB_SIZE = 3, MCB_NAME = 8, MCB_NAME_LEN = 8 };

enum {
    DE_OK = 0, DE_FUNCTION = 1, DE_FILE_NOT_FOUND = 2, DE_PATH_NOT_FOUND = 3,
    DE_TOO_MANY_OPEN = 4, DE_ACCESS_DENIED = 5, DE_INVALID_HANDLE = 6, DE_MCB_DESTROYED = 7,
    DE_NO_MEMORY = 8, DE_INVALID_BLOCK = 9, DE_BAD_ENV = 0x0A, DE_BAD_FORMAT = 0x0B,
    DE_ACCESS_CODE = 0x0C, DE_INVALID_DRIVE = 0x0F, DE_CURRENT_DIR = 0x10,
    DE_NOT_SAME_DEVICE = 0x11, DE_NO_MORE_FILES = 0x12, DE_FILE_EXISTS = 0x50,
};

/* File attributes. */
enum { ATTR_RDONLY = 0x01, ATTR_HIDDEN = 0x02, ATTR_SYSTEM = 0x04, ATTR_VOLUME = 0x08,
       ATTR_DIR = 0x10, ATTR_ARCHIVE = 0x20 };

typedef enum DosDevice { DEV_NONE, DEV_CON, DEV_NUL, DEV_AUX, DEV_PRN, DEV_CLOCK } DosDevice;

/* One system file table entry (host side). */
typedef struct DosFile {
    int refs; /* 0: slot free */
    int fd;   /* host descriptor, -1 for devices */
    DosDevice dev;
    uint8_t mode; /* open mode byte as passed to 3Dh */
    char dos_path[DOS_PATH_MAX];
    bool written; /* a write succeeded since the open: closing it reports the path */
} DosFile;

/* Parent state saved by EXEC, restored when the child terminates. */
typedef struct ExecFrame {
    uint16_t child_psp;
    bool from_shell; /* started by dos_start_program: no guest caller to return to */
    GpReg r[8];
    uint16_t seg[6];
} ExecFrame;

/* A resolved DOS path: canonical "C:\DIR\NAME.EXT" plus the host path it maps to. */
typedef struct DosPath {
    char dos[DOS_PATH_MAX];
    char host[HOST_PATH_MAX];
    bool exists;     /* host object exists */
    bool is_dir;
    DosDevice dev;   /* reserved device name (CON, NUL, ...) */
} DosPath;

/* The overlays of the last overlaid program whose own EXE was read (dos_modules.c). */
enum { DOS_OVERLAYS_MAX = 160 };
typedef struct DosOverlayCache {
    char dos_path[DOS_PATH_MAX]; /* "" when empty */
    int count;                    /* 0: the file has no overlays */
    BorlandOverlay list[DOS_OVERLAYS_MAX];
} DosOverlayCache;

typedef struct DosState {
    char host_root[HOST_PATH_MAX];
    char cwd[DOS_PATH_MAX]; /* of C:, "\" or "\DIR\SUB" */
    uint8_t cur_drive;
    uint16_t cur_psp, shell_psp;
    uint16_t dta_seg, dta_off;
    uint16_t last_error, return_code;
    uint8_t alloc_strategy;
    bool ctrl_break, verify;
    DosFile sft[DOS_SFT_MAX];
    ExecFrame frames[DOS_EXEC_DEPTH];
    int depth;
    char search_dirs[DOS_SEARCH_SLOTS][HOST_PATH_MAX];
    int search_next;
    uint8_t pending_scan; /* second byte of an extended key for AH=01/06/07/08 */
    bool flush_done;      /* AH=0Ch already emptied the buffer for the call being retried */
    char out_line[256];   /* console output collected until newline, then logged */
    int out_len;
    char in_line[256];    /* console line being typed for 0Ah / reads from CON */
    int in_len;
    char in_ready[258];   /* completed line waiting to be consumed by handle reads */
    int ready_len, ready_pos;
    time_t base_time;     /* host clock at init; DOS time advances with virtual_us */
    DosOverlayCache overlays;
    /* Native terminate learns the child is gone; the hook may longjmp and not return. */
    void (*terminate_hook)(Machine *m, uint8_t code);
} DosState;

static inline DosState *dos_state(Machine *m) { return (DosState *)m->dos; }

/* unlink and rename on the host, journaled during shadow verification (dos_snapshot.c). */
int dos_host_unlink(const char *path);
int dos_host_rename(const char *from, const char *to);
/* close(2) for DOS handles; deferred during a shadow run for descriptors open when it began. */
void dos_host_close(int fd);
/* A file the program wrote was closed, or renamed onto dos_path (browser build: Module.onFileClosed). */
void dos_host_file_closed(const char *dos_path);
/* open(2) for DOS open/create; exists says whether the path existed before. */
int dos_host_open(const char *path, int flags, bool exists);

typedef CallbackResult (*DosHandler)(Machine *m);

/* ---- results ---- */

CallbackResult dos_ok(Machine *m);
CallbackResult dos_fail(Machine *m, uint16_t err);

/* ---- paths (dos_path.c) ---- */

/* Resolves a guest file name; DE_OK or DE_PATH_NOT_FOUND / DE_INVALID_DRIVE. */
int dos_resolve(Machine *m, const char *name, DosPath *out);
void dos_read_name(Machine *m, uint16_t seg, uint16_t off, char *dst);
bool dos_name_to_fcb(const char *name, char fcb[11]);
bool dos_fcb_match(const char pattern[11], const char name[11]);
void dos_update_cds(Machine *m);
CallbackResult dos_select_drive(Machine *m);
CallbackResult dos_get_drive(Machine *m);
CallbackResult dos_mkdir(Machine *m);
CallbackResult dos_rmdir(Machine *m);
CallbackResult dos_chdir(Machine *m);
CallbackResult dos_getcwd(Machine *m);
CallbackResult dos_truename(Machine *m);
CallbackResult dos_free_space(Machine *m);
CallbackResult dos_drive_data(Machine *m);
CallbackResult dos_drive_data_default(Machine *m);

/* ---- files and handles (dos_file.c, dos_handle.c) ---- */

void dos_files_init(Machine *m);
void dos_files_shutdown(Machine *m);
DosFile *dos_handle_file(Machine *m, uint16_t handle);
int dos_jft_get(Machine *m, uint16_t psp, uint16_t handle);
void dos_jft_set(Machine *m, uint16_t psp, uint16_t handle, uint8_t sft);
uint16_t dos_jft_size(Machine *m, uint16_t psp);
void dos_inherit_handles(Machine *m, uint16_t parent, uint16_t child);
void dos_close_all(Machine *m, uint16_t psp);
void dos_sft_release(Machine *m, int idx);
int dos_alloc_handle(Machine *m, int sft_idx); /* -1: JFT full */
int dos_sft_alloc(Machine *m);                 /* -1: SFT full */
CallbackResult dos_create(Machine *m);
CallbackResult dos_open(Machine *m);
CallbackResult dos_close(Machine *m);
CallbackResult dos_read(Machine *m);
CallbackResult dos_write(Machine *m);
CallbackResult dos_delete(Machine *m);
CallbackResult dos_lseek(Machine *m);
CallbackResult dos_attrib(Machine *m);
CallbackResult dos_ioctl(Machine *m);
CallbackResult dos_dup(Machine *m);
CallbackResult dos_dup2(Machine *m);
CallbackResult dos_rename(Machine *m);
CallbackResult dos_file_time(Machine *m);
CallbackResult dos_create_temp(Machine *m);
CallbackResult dos_create_new(Machine *m);
CallbackResult dos_set_handle_count(Machine *m);
CallbackResult dos_commit(Machine *m);
CallbackResult dos_ext_open(Machine *m);

/* ---- directory search and FCB parsing (dos_find.c) ---- */

void dos_unix_to_dos_time(time_t t, uint16_t *date, uint16_t *time);
CallbackResult dos_set_dta(Machine *m);
CallbackResult dos_get_dta(Machine *m);
CallbackResult dos_find_first(Machine *m);
CallbackResult dos_find_next(Machine *m);
CallbackResult dos_parse_fcb(Machine *m);
/* Parses one name at s into an unopened FCB; returns chars consumed, *result as AL of 29h. */
int dos_parse_name_fcb(const char *s, uint8_t flags, uint8_t fcb[12], uint8_t *result);

/* ---- memory arena (dos_mem.c) ---- */

void dos_mem_init(Machine *m, uint16_t first, uint16_t end);
int dos_mem_alloc(Machine *m, uint16_t paras, uint16_t owner, uint16_t *seg, uint16_t *largest);
int dos_mem_free(Machine *m, uint16_t seg);
int dos_mem_resize(Machine *m, uint16_t seg, uint16_t paras, uint16_t *largest);
void dos_mem_free_owner(Machine *m, uint16_t owner);
void dos_mem_set_owner(Machine *m, uint16_t seg, uint16_t owner, const char *name);
uint16_t dos_mem_block_size(Machine *m, uint16_t seg);
CallbackResult dos_alloc(Machine *m);
CallbackResult dos_free(Machine *m);
CallbackResult dos_resize(Machine *m);
CallbackResult dos_strategy(Machine *m);

/* ---- processes (dos_exec.c, dos_proc.c) ---- */

void dos_build_psp(Machine *m, uint16_t psp, uint16_t parent, uint16_t top, uint16_t env);
void dos_create_shell(Machine *m);
void dos_terminate(Machine *m, uint8_t code, uint8_t type, int keep_paras);
CallbackResult dos_exec(Machine *m);
CallbackResult dos_exit(Machine *m);
CallbackResult dos_exit_old(Machine *m);
CallbackResult dos_keep(Machine *m);
CallbackResult dos_get_return_code(Machine *m);
CallbackResult dos_set_psp(Machine *m);
CallbackResult dos_get_psp(Machine *m);
CallbackResult dos_new_psp(Machine *m);
CallbackResult dos_child_psp(Machine *m);
CallbackResult dos_int20(Machine *m);
CallbackResult dos_int27(Machine *m);

/* ---- console (dos_con.c) ---- */

void dos_con_write(Machine *m, const uint8_t *buf, size_t len);
/* Reads from CON in cooked line mode; false: no complete line yet (retry later). */
bool dos_con_read(Machine *m, uint8_t *buf, uint16_t cap, uint16_t *got);
bool dos_con_key_ready(Machine *m);
CallbackResult dos_con_in_echo(Machine *m);
CallbackResult dos_con_out(Machine *m);
CallbackResult dos_aux_in(Machine *m);
CallbackResult dos_aux_out(Machine *m);
CallbackResult dos_prn_out(Machine *m);
CallbackResult dos_direct_con(Machine *m);
CallbackResult dos_con_in_raw(Machine *m);
CallbackResult dos_con_in(Machine *m);
CallbackResult dos_print_string(Machine *m);
CallbackResult dos_buffered_input(Machine *m);
CallbackResult dos_con_status(Machine *m);
CallbackResult dos_con_flush(Machine *m);
CallbackResult dos_int29(Machine *m);

/* ---- system information (dos_info.c) ---- */

void dos_info_init(Machine *m);
CallbackResult dos_disk_reset(Machine *m);
CallbackResult dos_set_vector(Machine *m);
CallbackResult dos_get_vector(Machine *m);
CallbackResult dos_get_date(Machine *m);
CallbackResult dos_set_date(Machine *m);
CallbackResult dos_get_time(Machine *m);
CallbackResult dos_set_time(Machine *m);
CallbackResult dos_set_verify(Machine *m);
CallbackResult dos_get_verify(Machine *m);
CallbackResult dos_version(Machine *m);
CallbackResult dos_ctrl_break(Machine *m);
CallbackResult dos_indos(Machine *m);
CallbackResult dos_switch_char(Machine *m);
CallbackResult dos_country(Machine *m);
CallbackResult dos_list_of_lists(Machine *m);
CallbackResult dos_ext_error(Machine *m);
CallbackResult dos_dbcs(Machine *m);
CallbackResult dos_ext_country(Machine *m);
CallbackResult dos_code_page(Machine *m);
CallbackResult dos_get_dpb(Machine *m);

/* A program read part of a file into memory; announces driver and DLL images to the lift registry. */
void dos_note_module_read(Machine *m, const DosFile *f, off_t file_pos, uint16_t seg, uint16_t off,
                          uint16_t bytes);

#endif
