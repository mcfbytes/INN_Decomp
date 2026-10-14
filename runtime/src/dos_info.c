/* Kernel data in guest memory (SYSVARS, NUL device, InDOS, DPB, NLS tables) and the INT 21h
 * functions that only report or set system state: version, vectors, date and time, country. */
#define _POSIX_C_SOURCE 200809L /* fds, directories, localtime_r on any libc */
#include <ctype.h>
#include <string.h>
#include <time.h>

#include "dos_int.h"

enum { DOS_MAJOR = 5, DOS_MINOR = 0, OEM_MICROSOFT = 0xFF, COUNTRY_USA = 1, CODE_PAGE = 437 };
enum { MEDIA_FIXED = 0xF8, NUL_ATTR = 0x8004, SECTOR_BYTES = 512, OP_RETF = 0xCB };
enum { DD_RETF = 0x0110 - 2 }; /* a far RETF used as the country case-map routine */

/* SYSVARS (List of Lists) fields, relative to DD_LOL. */
enum {
    LOL_DPB = 0x00, LOL_SFT = 0x04, LOL_CLOCK = 0x08, LOL_CON = 0x0C, LOL_SECTOR = 0x10,
    LOL_CDS = 0x16, LOL_BLOCK_DEVS = 0x20, LOL_LASTDRIVE = 0x21, LOL_NUL = 0x22,
};

static void wr_far(Machine *m, uint32_t at, uint16_t seg, uint16_t off)
{
    mem_wr16(m, at, off);
    mem_wr16(m, at + 2, seg);
}

static void build_sysvars(Machine *m)
{
    uint32_t lol = real_addr(DOS_DATA_SEG, DD_LOL);
    wr_far(m, lol + LOL_DPB, DOS_DATA_SEG, DD_DPB);
    wr_far(m, lol + LOL_SFT, DOS_DATA_SEG, DD_SFT);
    wr_far(m, lol + LOL_CLOCK, DOS_DATA_SEG, DD_NUL_DEV);
    wr_far(m, lol + LOL_CON, DOS_DATA_SEG, DD_NUL_DEV);
    mem_wr16(m, lol + LOL_SECTOR, SECTOR_BYTES);
    wr_far(m, lol + LOL_CDS, DOS_DATA_SEG, DD_CDS);
    mem_wr8(m, lol + LOL_BLOCK_DEVS, 1);
    mem_wr8(m, lol + LOL_LASTDRIVE, DOS_LASTDRIVE);
    uint32_t nul = real_addr(DOS_DATA_SEG, DD_NUL_DEV);
    mem_wr32(m, nul, 0xFFFFFFFFu);
    mem_wr16(m, nul + 4, NUL_ATTR);
    mem_write_block(m, nul + 10, "NUL     ", 8);
}

/* Enough of a drive parameter block for programs that look at the cluster geometry. */
static void build_dpb(Machine *m)
{
    uint32_t dpb = real_addr(DOS_DATA_SEG, DD_DPB);
    mem_wr8(m, dpb + 0x00, DOS_DRIVE_C);
    mem_wr16(m, dpb + 0x02, SECTOR_BYTES);
    mem_wr8(m, dpb + 0x04, 63);   /* sectors per cluster - 1 */
    mem_wr8(m, dpb + 0x05, 6);    /* cluster-to-sector shift */
    mem_wr8(m, dpb + 0x08, 2);    /* FAT copies */
    mem_wr16(m, dpb + 0x09, 512); /* root entries */
    mem_wr16(m, dpb + 0x0D, 0xFFF1);
    mem_wr8(m, dpb + 0x17, MEDIA_FIXED);
    mem_wr32(m, dpb + 0x19, 0xFFFFFFFFu);
    mem_wr8(m, real_addr(DOS_DATA_SEG, DD_MEDIA_ID), MEDIA_FIXED);
}

static const char file_terminators[] = ".\"/\\[]:|<>+=;,";

static void build_nls_tables(Machine *m)
{
    uint32_t up = real_addr(DOS_DATA_SEG, DD_UPCASE);
    mem_wr16(m, up, 0x80);
    for (int i = 0; i < 0x80; i++)
        mem_wr8(m, up + 2 + (uint32_t)i, (uint8_t)(0x80 + i));
    uint32_t col = real_addr(DOS_DATA_SEG, DD_COLLATE);
    mem_wr16(m, col, 0x100);
    for (int i = 0; i < 0x100; i++)
        mem_wr8(m, col + 2 + (uint32_t)i, (uint8_t)toupper(i));
    uint32_t ft = real_addr(DOS_DATA_SEG, DD_FILECHARS);
    static const uint8_t head[] = {1, 0x00, 0xFF, 0, 0x00, 0x20, 2};
    mem_wr16(m, ft, (uint16_t)(sizeof head + 1 + strlen(file_terminators)));
    mem_write_block(m, ft + 2, head, sizeof head);
    mem_wr8(m, ft + 2 + sizeof head, (uint8_t)strlen(file_terminators));
    mem_write_block(m, ft + 3 + sizeof head, file_terminators, strlen(file_terminators));
    mem_wr16(m, real_addr(DOS_DATA_SEG, DD_DBCS), 0);
    mem_wr8(m, real_addr(DOS_DATA_SEG, DD_RETF), OP_RETF);
}

void dos_info_init(Machine *m)
{
    build_sysvars(m);
    build_dpb(m);
    build_nls_tables(m);
    mem_wr8(m, real_addr(DOS_DATA_SEG, DD_INDOS), 0);
    mem_wr8(m, real_addr(DOS_DATA_SEG, DD_CRIT_ERR), 0);
}

CallbackResult dos_disk_reset(Machine *m) { return CB_DONE; }

CallbackResult dos_set_vector(Machine *m)
{
    mem_wr16(m, (uint32_t)AL * 4, DX);
    mem_wr16(m, (uint32_t)AL * 4 + 2, DS);
    return CB_DONE;
}

CallbackResult dos_get_vector(Machine *m)
{
    BX = mem_rd16(m, (uint32_t)AL * 4);
    cpu_set_seg(&m->cpu, S_ES, mem_rd16(m, (uint32_t)AL * 4 + 2));
    return CB_DONE;
}

/* ---- date and time: the host clock at start, advanced by emulated time ---- */

static void now(Machine *m, struct tm *tm, int *hundredths)
{
    uint64_t us = m->virtual_us;
    time_t t = dos_state(m)->base_time + (time_t)(us / 1000000);
    localtime_r(&t, tm);
    *hundredths = (int)(us / 10000 % 100);
}

CallbackResult dos_get_date(Machine *m)
{
    struct tm tm;
    int hs;
    now(m, &tm, &hs);
    CX = (uint16_t)(tm.tm_year + 1900);
    DH = (uint8_t)(tm.tm_mon + 1);
    DL = (uint8_t)tm.tm_mday;
    AL = (uint8_t)tm.tm_wday;
    return CB_DONE;
}

CallbackResult dos_get_time(Machine *m)
{
    struct tm tm;
    int hs;
    now(m, &tm, &hs);
    CH = (uint8_t)tm.tm_hour;
    CL = (uint8_t)tm.tm_min;
    DH = (uint8_t)tm.tm_sec;
    DL = (uint8_t)hs;
    return CB_DONE;
}

/* The guest may not move the host clock; accept and ignore. */
CallbackResult dos_set_date(Machine *m)
{
    AL = 0;
    return CB_DONE;
}

CallbackResult dos_set_time(Machine *m)
{
    AL = 0;
    return CB_DONE;
}

CallbackResult dos_set_verify(Machine *m)
{
    dos_state(m)->verify = AL != 0;
    return CB_DONE;
}

CallbackResult dos_get_verify(Machine *m)
{
    AL = dos_state(m)->verify;
    return CB_DONE;
}

/* ---- version and flags ---- */

CallbackResult dos_version(Machine *m)
{
    BH = AL == 1 ? 0 : OEM_MICROSOFT;
    AL = DOS_MAJOR;
    AH = DOS_MINOR;
    BL = 0;
    CX = 0;
    return CB_DONE;
}

enum { BRK_GET = 0, BRK_SET = 1, BRK_SWAP = 2, BRK_BOOT_DRIVE = 5, BRK_TRUE_VERSION = 6 };

CallbackResult dos_ctrl_break(Machine *m)
{
    DosState *d = dos_state(m);
    bool old = d->ctrl_break;
    switch (AL) {
    case BRK_GET: DL = old; break;
    case BRK_SET: d->ctrl_break = DL != 0; break;
    case BRK_SWAP: d->ctrl_break = DL != 0; DL = old; break;
    case BRK_BOOT_DRIVE: DL = DOS_DRIVE_C + 1; break;
    case BRK_TRUE_VERSION: BL = DOS_MAJOR; BH = DOS_MINOR; DX = 0; break;
    default: AL = 0xFF; break;
    }
    return CB_DONE;
}

CallbackResult dos_indos(Machine *m)
{
    cpu_set_seg(&m->cpu, S_ES, DOS_DATA_SEG);
    BX = DD_INDOS;
    return CB_DONE;
}

CallbackResult dos_switch_char(Machine *m)
{
    if (AL == 0)
        DL = '/';
    AL = 0;
    return CB_DONE;
}

CallbackResult dos_list_of_lists(Machine *m)
{
    cpu_set_seg(&m->cpu, S_ES, DOS_DATA_SEG);
    BX = DD_LOL;
    return CB_DONE;
}

/* Error class / suggested action / locus for AH=59h, by error code. */
CallbackResult dos_ext_error(Machine *m)
{
    uint16_t err = dos_state(m)->last_error;
    enum { CLASS_RESOURCE = 1, CLASS_AUTH = 3, CLASS_APP = 7, CLASS_NOT_FOUND = 8 };
    enum { ACTION_ABORT = 4, ACTION_USER = 6, LOCUS_UNKNOWN = 1, LOCUS_BLOCK = 2, LOCUS_MEM = 5 };
    uint8_t cls = CLASS_APP, locus = LOCUS_UNKNOWN;
    if (err == DE_FILE_NOT_FOUND || err == DE_PATH_NOT_FOUND || err == DE_NO_MORE_FILES)
        cls = CLASS_NOT_FOUND, locus = LOCUS_BLOCK;
    else if (err == DE_TOO_MANY_OPEN || err == DE_NO_MEMORY)
        cls = CLASS_RESOURCE, locus = err == DE_NO_MEMORY ? LOCUS_MEM : LOCUS_UNKNOWN;
    else if (err == DE_ACCESS_DENIED)
        cls = CLASS_AUTH, locus = LOCUS_BLOCK;
    AX = err;
    BH = cls;
    BL = cls == CLASS_NOT_FOUND ? ACTION_USER : ACTION_ABORT;
    CH = locus;
    return CB_DONE;
}

/* ---- country information ---- */

enum { COUNTRY_INFO_LEN = 34, CI_CASEMAP = 0x12 };

static void write_country_info(Machine *m, uint32_t at)
{
    static const uint8_t usa[COUNTRY_INFO_LEN] = {
        0, 0, '$', 0, 0, 0, 0, ',', 0, '.', 0, '-', 0, ':', 0, 0, 2, 0,
        0, 0, 0, 0, ',', 0,
    };
    mem_write_block(m, at, usa, sizeof usa);
    wr_far(m, at + CI_CASEMAP, DOS_DATA_SEG, DD_RETF);
}

CallbackResult dos_country(Machine *m)
{
    if (DX == 0xFFFF)
        return dos_ok(m); /* set country: only one is available, accept it */
    write_country_info(m, real_addr(DS, DX));
    AX = BX = COUNTRY_USA;
    return dos_ok(m);
}

CallbackResult dos_dbcs(Machine *m)
{
    if (AL != 0)
        return dos_fail(m, DE_FUNCTION);
    cpu_set_seg(&m->cpu, S_DS, DOS_DATA_SEG);
    SI = DD_DBCS + 2; /* empty list: the terminating zero word */
    mem_wr16(m, real_addr(DOS_DATA_SEG, DD_DBCS + 2), 0);
    AL = 0;
    return dos_ok(m);
}

enum { EXT_INFO_HDR = 7 };

static CallbackResult table_pointer(Machine *m, uint16_t off)
{
    uint32_t dst = real_addr(ES, DI);
    mem_wr8(m, dst, AL);
    wr_far(m, dst + 1, DOS_DATA_SEG, off);
    CX = 5;
    return dos_ok(m);
}

static void upcase_block(Machine *m, uint32_t at, uint32_t len, bool asciiz)
{
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = mem_rd8(m, at + i);
        if (asciiz && !c)
            break;
        mem_wr8(m, at + i, (uint8_t)toupper(c));
    }
}

CallbackResult dos_ext_country(Machine *m)
{
    switch (AL) {
    case 0x01: {
        uint32_t dst = real_addr(ES, DI);
        mem_wr8(m, dst, 1);
        mem_wr16(m, dst + 1, EXT_INFO_HDR - 3 + COUNTRY_INFO_LEN);
        mem_wr16(m, dst + 3, COUNTRY_USA);
        mem_wr16(m, dst + 5, CODE_PAGE);
        write_country_info(m, dst + EXT_INFO_HDR);
        CX = EXT_INFO_HDR + COUNTRY_INFO_LEN;
        return dos_ok(m);
    }
    case 0x02: case 0x04: return table_pointer(m, DD_UPCASE);
    case 0x05: return table_pointer(m, DD_FILECHARS);
    case 0x06: return table_pointer(m, DD_COLLATE);
    case 0x07: return table_pointer(m, DD_DBCS);
    case 0x20: DL = (uint8_t)toupper(DL); return dos_ok(m);
    case 0x21: upcase_block(m, real_addr(DS, DX), CX, false); return dos_ok(m);
    case 0x22: upcase_block(m, real_addr(DS, DX), 0xFFFF, true); return dos_ok(m);
    }
    return dos_fail(m, DE_FUNCTION);
}

CallbackResult dos_code_page(Machine *m)
{
    if (AL == 1)
        BX = DX = CODE_PAGE;
    return dos_ok(m);
}

CallbackResult dos_get_dpb(Machine *m)
{
    if (AH == 0x32 && DL != 0 && DL != DOS_DRIVE_C + 1) {
        AL = 0xFF;
        return CB_DONE;
    }
    cpu_set_seg(&m->cpu, S_DS, DOS_DATA_SEG);
    BX = DD_DPB;
    AL = 0;
    return CB_DONE;
}
