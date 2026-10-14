/* DOS kernel entry: initialisation, the INT 21h dispatch table and the small service interrupts
 * (INT 23h, 24h, 28h, 2Fh, 67h) that DOS owns. */
#define _POSIX_C_SOURCE 200809L /* fds, directories, localtime_r on any libc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dos_int.h"

/* How the dispatcher finds a function's file name argument, for tracing. */
typedef enum NameArg { NAME_NONE, NAME_DS_DX, NAME_DS_SI } NameArg;

typedef struct DosFunction {
    DosHandler fn;
    const char *name;
    NameArg name_arg;
} DosFunction;

static const DosFunction dos_functions[256] = {
    [0x00] = {dos_exit_old, "terminate", NAME_NONE},
    [0x01] = {dos_con_in_echo, "read char with echo", NAME_NONE},
    [0x02] = {dos_con_out, "write char", NAME_NONE},
    [0x03] = {dos_aux_in, "aux input", NAME_NONE},
    [0x04] = {dos_aux_out, "aux output", NAME_NONE},
    [0x05] = {dos_prn_out, "printer output", NAME_NONE},
    [0x06] = {dos_direct_con, "direct console i/o", NAME_NONE},
    [0x07] = {dos_con_in_raw, "direct char input", NAME_NONE},
    [0x08] = {dos_con_in, "char input no echo", NAME_NONE},
    [0x09] = {dos_print_string, "print string", NAME_NONE},
    [0x0A] = {dos_buffered_input, "buffered input", NAME_NONE},
    [0x0B] = {dos_con_status, "input status", NAME_NONE},
    [0x0C] = {dos_con_flush, "flush and input", NAME_NONE},
    [0x0D] = {dos_disk_reset, "disk reset", NAME_NONE},
    [0x0E] = {dos_select_drive, "select drive", NAME_NONE},
    [0x19] = {dos_get_drive, "get drive", NAME_NONE},
    [0x1A] = {dos_set_dta, "set dta", NAME_NONE},
    [0x1B] = {dos_drive_data_default, "default drive data", NAME_NONE},
    [0x1C] = {dos_drive_data, "drive data", NAME_NONE},
    [0x1F] = {dos_get_dpb, "default dpb", NAME_NONE},
    [0x25] = {dos_set_vector, "set vector", NAME_NONE},
    [0x26] = {dos_new_psp, "create psp", NAME_NONE},
    [0x29] = {dos_parse_fcb, "parse filename", NAME_DS_SI},
    [0x2A] = {dos_get_date, "get date", NAME_NONE},
    [0x2B] = {dos_set_date, "set date", NAME_NONE},
    [0x2C] = {dos_get_time, "get time", NAME_NONE},
    [0x2D] = {dos_set_time, "set time", NAME_NONE},
    [0x2E] = {dos_set_verify, "set verify", NAME_NONE},
    [0x2F] = {dos_get_dta, "get dta", NAME_NONE},
    [0x30] = {dos_version, "version", NAME_NONE},
    [0x31] = {dos_keep, "keep process", NAME_NONE},
    [0x32] = {dos_get_dpb, "dpb", NAME_NONE},
    [0x33] = {dos_ctrl_break, "ctrl-break", NAME_NONE},
    [0x34] = {dos_indos, "indos address", NAME_NONE},
    [0x35] = {dos_get_vector, "get vector", NAME_NONE},
    [0x36] = {dos_free_space, "free space", NAME_NONE},
    [0x37] = {dos_switch_char, "switch char", NAME_NONE},
    [0x38] = {dos_country, "country info", NAME_NONE},
    [0x39] = {dos_mkdir, "mkdir", NAME_DS_DX},
    [0x3A] = {dos_rmdir, "rmdir", NAME_DS_DX},
    [0x3B] = {dos_chdir, "chdir", NAME_DS_DX},
    [0x3C] = {dos_create, "create", NAME_DS_DX},
    [0x3D] = {dos_open, "open", NAME_DS_DX},
    [0x3E] = {dos_close, "close", NAME_NONE},
    [0x3F] = {dos_read, "read", NAME_NONE},
    [0x40] = {dos_write, "write", NAME_NONE},
    [0x41] = {dos_delete, "delete", NAME_DS_DX},
    [0x42] = {dos_lseek, "lseek", NAME_NONE},
    [0x43] = {dos_attrib, "attributes", NAME_DS_DX},
    [0x44] = {dos_ioctl, "ioctl", NAME_NONE},
    [0x45] = {dos_dup, "dup", NAME_NONE},
    [0x46] = {dos_dup2, "force dup", NAME_NONE},
    [0x47] = {dos_getcwd, "getcwd", NAME_NONE},
    [0x48] = {dos_alloc, "alloc", NAME_NONE},
    [0x49] = {dos_free, "free", NAME_NONE},
    [0x4A] = {dos_resize, "resize", NAME_NONE},
    [0x4B] = {dos_exec, "exec", NAME_DS_DX},
    [0x4C] = {dos_exit, "exit", NAME_NONE},
    [0x4D] = {dos_get_return_code, "return code", NAME_NONE},
    [0x4E] = {dos_find_first, "find first", NAME_DS_DX},
    [0x4F] = {dos_find_next, "find next", NAME_NONE},
    [0x50] = {dos_set_psp, "set psp", NAME_NONE},
    [0x51] = {dos_get_psp, "get psp", NAME_NONE},
    [0x52] = {dos_list_of_lists, "list of lists", NAME_NONE},
    [0x54] = {dos_get_verify, "get verify", NAME_NONE},
    [0x55] = {dos_child_psp, "create child psp", NAME_NONE},
    [0x56] = {dos_rename, "rename", NAME_DS_DX},
    [0x57] = {dos_file_time, "file date/time", NAME_NONE},
    [0x58] = {dos_strategy, "alloc strategy", NAME_NONE},
    [0x59] = {dos_ext_error, "extended error", NAME_NONE},
    [0x5A] = {dos_create_temp, "create temp", NAME_DS_DX},
    [0x5B] = {dos_create_new, "create new", NAME_DS_DX},
    [0x60] = {dos_truename, "truename", NAME_DS_SI},
    [0x62] = {dos_get_psp, "get psp", NAME_NONE},
    [0x63] = {dos_dbcs, "dbcs table", NAME_NONE},
    [0x65] = {dos_ext_country, "ext country", NAME_NONE},
    [0x66] = {dos_code_page, "code page", NAME_NONE},
    [0x67] = {dos_set_handle_count, "set handle count", NAME_NONE},
    [0x68] = {dos_commit, "commit", NAME_NONE},
    [0x6A] = {dos_commit, "commit", NAME_NONE},
    [0x6C] = {dos_ext_open, "extended open", NAME_DS_SI},
};

CallbackResult dos_ok(Machine *m)
{
    cb_set_carry(m, false);
    return CB_DONE;
}

CallbackResult dos_fail(Machine *m, uint16_t err)
{
    dos_state(m)->last_error = err;
    AX = err;
    cb_set_carry(m, true);
    return CB_DONE;
}

static void trace_call(Machine *m, const DosFunction *f)
{
    char name[DOS_PATH_MAX] = "";
    if (f->name_arg == NAME_DS_DX)
        dos_read_name(m, DS, DX, name);
    else if (f->name_arg == NAME_DS_SI)
        dos_read_name(m, DS, SI, name);
    uint32_t frame = m->cpu.seg_base[S_SS] + SP;
    log_msg("int21 %04X:%04X AH=%02X AL=%02X BX=%04X CX=%04X DX=%04X %s%s%s%s",
            mem_rd16(m, frame + 2), mem_rd16(m, frame), AH, AL, BX, CX, DX,
            f->name ? f->name : "unsupported", name[0] ? " \"" : "", name, name[0] ? "\"" : "");
}

static CallbackResult int21(Machine *m)
{
    const DosFunction *f = &dos_functions[AH];
    if (m->trace >= 1)
        trace_call(m, f);
    if (!f->fn) {
        if (m->trace < 1)
            log_msg("dos: unsupported INT 21h AH=%02X AL=%02X", AH, AL);
        AL = 0;
        return dos_fail(m, DE_FUNCTION);
    }
    return f->fn(m);
}

/* ---- INT 23h / 24h / 28h defaults ---- */

static CallbackResult int22_shell(Machine *m)
{
    /* Only reached if a program jumps to its terminate address by hand. */
    m->exit_requested = true;
    return CB_DONE;
}

static CallbackResult int23_default(Machine *m)
{
    return CB_DONE; /* ignore Ctrl-C rather than killing the program */
}

static CallbackResult int24_default(Machine *m)
{
    AL = 3; /* fail the call */
    return CB_DONE;
}

static CallbackResult int28_idle(Machine *m) { return CB_DONE; }

/* ---- INT 2Fh multiplex ---- */

enum { MUX_WINDOWS = 0x1600, MUX_XMS = 0x4300, MUX_IDLE = 0x1680 };

static CallbackResult int2f(Machine *m)
{
    switch (AX) {
    case MUX_WINDOWS:
        AL = 0; /* no enhanced-mode Windows */
        break;
    case MUX_XMS:
        AL = 0; /* XMS driver not installed */
        break;
    case MUX_IDLE:
        AL = 0; /* timeslice released */
        break;
    default:
        if (m->trace)
            log_msg("dos: INT 2Fh AX=%04X passed", AX);
        break;
    }
    return CB_DONE;
}

/* ---- INT 67h: no expanded memory ---- */

enum { EMS_NOT_SUPPORTED = 0x84 };

static CallbackResult int67(Machine *m)
{
    if (m->trace)
        log_msg("dos: INT 67h AH=%02X (no EMS)", AH);
    AH = EMS_NOT_SUPPORTED;
    return CB_DONE;
}

/* ---- init ---- */

typedef struct VectorRow {
    const char *name;
    CallbackFn fn;
    int vector;
} VectorRow;

static const VectorRow dos_vectors[] = {
    {"dos int20", dos_int20, 0x20},     {"dos int21", int21, 0x21},
    {"dos int22", int22_shell, 0x22},   {"dos int23", int23_default, 0x23},
    {"dos int24", int24_default, 0x24}, {"dos int27", dos_int27, 0x27},
    {"dos int28", int28_idle, 0x28},    {"dos int29", dos_int29, 0x29},
    {"dos int2f", int2f, 0x2F},         {"dos int67", int67, 0x67},
};

void dos_init(Machine *m, const char *c_drive_host_dir)
{
    DosState *d = calloc(1, sizeof *d);
    if (!d)
        fatal("out of memory");
    m->dos = d;
    snprintf(d->host_root, sizeof d->host_root, "%s", c_drive_host_dir);
    size_t n = strlen(d->host_root);
    while (n > 1 && d->host_root[n - 1] == '/')
        d->host_root[--n] = 0;
    strcpy(d->cwd, "\\");
    d->cur_drive = DOS_DRIVE_C;
    d->ctrl_break = false;
    d->base_time = time(NULL);
    for (size_t i = 0; i < sizeof dos_vectors / sizeof dos_vectors[0]; i++)
        callback_register(m, dos_vectors[i].name, dos_vectors[i].fn, dos_vectors[i].vector);
    dos_info_init(m);
    dos_mem_init(m, DOS_FIRST_MCB, DOS_ARENA_END);
    dos_files_init(m);
    dos_update_cds(m);
    dos_create_shell(m);
}

void dos_shutdown(Machine *m)
{
    if (!m->dos)
        return;
    dos_files_shutdown(m);
    free(m->dos);
    m->dos = NULL;
}
