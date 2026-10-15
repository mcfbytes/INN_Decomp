/* BIOS: ROM image (fonts, trampolines, model byte), IVT and BDA setup, and the small services
 * (INT 08h timer, 11h, 12h, 13h, 15h, 17h, 1Ah). Video is bios_video.c, keyboard bios_kbd.c. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "bios_internal.h"
#include "font.h"

enum {
    IVT_VECTORS = 256,
    TICKS_PER_DAY = 0x1800B0,
    EQUIP_FLOPPY = 0x0001, EQUIP_FPU = 0x0002, EQUIP_MOUSE = 0x0004, EQUIP_VIDEO_COLOR80 = 0x0020,
    EQUIP_SERIAL_SHIFT = 9, SERIAL_PORTS = 2,
    CONVENTIONAL_KB = 640,
    COM1_BASE = 0x3F8, COM2_BASE = 0x2F8,
    VEC_TIMER = 0x08, VEC_IRQ7 = 0x0F, VEC_IRQ8 = 0x70, VEC_IRQ15 = 0x77,
    VEC_EQUIPMENT = 0x11, VEC_MEMORY = 0x12, VEC_DISK = 0x13, VEC_SYSTEM = 0x15,
    VEC_PRINTER = 0x17, VEC_TIME = 0x1A, VEC_USER_TICK = 0x1C, VEC_FONT_HIGH = 0x1F,
    VEC_FONT_GRAPHICS = 0x43,
    PRINTER_READY = 0x90,
    ERR_UNSUPPORTED = 0x86, DISK_ERR_BAD_COMMAND = 0x01,
};

typedef struct Bios {
    uint16_t rom_next;
    uint16_t config_table_off;
    uint64_t wait_until_us; /* INT 15h AH=86h in progress; 0 when idle */
} Bios;

static Bios *bios_state(Machine *m) { return m->bios; }

/* ---- ROM helpers ---- */

uint16_t bios_rom_emit(Machine *m, const uint8_t *code, size_t len)
{
    Bios *b = bios_state(m);
    if (b->rom_next + len > ROM_CODE_END)
        fatal("BIOS ROM code area full");
    uint16_t at = b->rom_next;
    memcpy(m->mem + real_addr(BIOS_SEG, at), code, len);
    b->rom_next = (uint16_t)(b->rom_next + len);
    return at;
}

uint16_t bios_rom_callback(Machine *m, const char *name, CallbackFn fn)
{
    int id = callback_register(m, name, fn, -1);
    const uint8_t trap[3] = { 0xFE, 0x38, (uint8_t)id };
    return bios_rom_emit(m, trap, sizeof trap);
}

void bios_set_vector(Machine *m, int vector, uint16_t seg, uint16_t off)
{
    mem_wr16(m, (uint32_t)vector * 4, off);
    mem_wr16(m, (uint32_t)vector * 4 + 2, seg);
}

/* push ax; mov al,20h; [out 0A0h,al;] out 20h,al; pop ax; iret */
static uint16_t emit_eoi_iret(Machine *m, bool slave)
{
    static const uint8_t MASTER[] = { 0x50, 0xB0, PIC_EOI_CMD, 0xE6, 0x20, 0x58, 0xCF };
    static const uint8_t SLAVE[] = { 0x50, 0xB0, PIC_EOI_CMD, 0xE6, 0xA0, 0xE6, 0x20, 0x58, 0xCF };
    return slave ? bios_rom_emit(m, SLAVE, sizeof SLAVE) : bios_rom_emit(m, MASTER, sizeof MASTER);
}

void bios_irq_handler(Machine *m, int vector, const char *name, CallbackFn fn, bool slave)
{
    uint16_t entry = bios_rom_callback(m, name, fn);
    emit_eoi_iret(m, slave);
    bios_set_vector(m, vector, BIOS_SEG, entry);
}

CallbackResult bios_block(Machine *m)
{
    m->cpu.flags |= F_IF;
    return CB_RETRY;
}

/* ---- INT 08h: timer tick ---- */

static CallbackResult int08_tick(Machine *m)
{
    uint32_t ticks = bda_rd16(m, BDA_TICKS) | (uint32_t)bda_rd16(m, BDA_TICKS + 2) << 16;
    if (++ticks >= TICKS_PER_DAY) {
        ticks = 0;
        bda_wr8(m, BDA_MIDNIGHT, 1);
    }
    bda_wr16(m, BDA_TICKS, (uint16_t)ticks);
    bda_wr16(m, BDA_TICKS + 2, (uint16_t)(ticks >> 16));
    return CB_DONE;
}

/* Tick bookkeeping, then INT 1Ch with the hardware interrupt still in service, then EOI. */
static void install_timer(Machine *m)
{
    uint16_t entry = bios_rom_callback(m, "int08", int08_tick);
    static const uint8_t CALL_1C[] = { 0xCD, VEC_USER_TICK };
    bios_rom_emit(m, CALL_1C, sizeof CALL_1C);
    emit_eoi_iret(m, false);
    bios_set_vector(m, VEC_TIMER, BIOS_SEG, entry);
}

/* ---- small services ---- */

static CallbackResult int11_equipment(Machine *m)
{
    AX = bda_rd16(m, BDA_EQUIPMENT);
    return CB_DONE;
}

static CallbackResult int12_memory(Machine *m)
{
    AX = bda_rd16(m, BDA_MEMORY_KB);
    return CB_DONE;
}

static CallbackResult int13_disk(Machine *m)
{
    AH = DISK_ERR_BAD_COMMAND;
    cb_set_carry(m, true);
    return CB_DONE;
}

static CallbackResult int17_printer(Machine *m)
{
    AH = PRINTER_READY;
    return CB_DONE;
}

static CallbackResult int15_wait(Machine *m)
{
    Bios *b = bios_state(m);
    if (!b->wait_until_us)
        b->wait_until_us = m->virtual_us + ((uint32_t)CX << 16 | DX);
    if (m->virtual_us < b->wait_until_us)
        return bios_block(m);
    b->wait_until_us = 0;
    cb_set_carry(m, false);
    return CB_DONE;
}

static CallbackResult int15_system(Machine *m)
{
    switch (AH) {
    case 0x4F: cb_set_carry(m, true); return CB_DONE; /* keyboard intercept: keep the key */
    case 0x86: return int15_wait(m);
    case 0x88: AX = 0; cb_set_carry(m, false); return CB_DONE;
    case 0xC0:
        cpu_set_seg(&m->cpu, S_ES, BIOS_SEG);
        BX = bios_state(m)->config_table_off;
        AH = 0;
        cb_set_carry(m, false);
        return CB_DONE;
    case 0x24:
        if (AL <= 1)
            m->a20 = AL;
        AL = AL == 2 ? m->a20 : AL == 3 ? 3 : AL;
        AH = 0;
        cb_set_carry(m, false);
        return CB_DONE;
    case 0x90: case 0x91: AH = 0; cb_set_carry(m, false); return CB_DONE;
    default: AH = ERR_UNSUPPORTED; cb_set_carry(m, true); return CB_DONE;
    }
}

static uint8_t to_bcd(int v) { return (uint8_t)((v / 10) << 4 | v % 10); }

static struct tm host_now(void)
{
    time_t t = time(NULL);
    struct tm tm;
    struct tm *lt = localtime(&t);
    if (lt)
        tm = *lt;
    else
        memset(&tm, 0, sizeof tm);
    return tm;
}

static CallbackResult int1a_time(Machine *m)
{
    /* Drivers poll AH=00h in tight loops and localtime is costly, so only RTC reads take it. */
    struct tm tm = AH == 0x02 || AH == 0x04 ? host_now() : (struct tm){ 0 };
    switch (AH) {
    case 0x00:
        CX = bda_rd16(m, BDA_TICKS + 2);
        DX = bda_rd16(m, BDA_TICKS);
        AL = bda_rd8(m, BDA_MIDNIGHT);
        bda_wr8(m, BDA_MIDNIGHT, 0);
        break;
    case 0x01:
        bda_wr16(m, BDA_TICKS + 2, CX);
        bda_wr16(m, BDA_TICKS, DX);
        break;
    case 0x02:
        CH = to_bcd(tm.tm_hour), CL = to_bcd(tm.tm_min), DH = to_bcd(tm.tm_sec), DL = 0;
        break;
    case 0x04:
        CH = to_bcd(19 + tm.tm_year / 100), CL = to_bcd(tm.tm_year % 100);
        DH = to_bcd(tm.tm_mon + 1), DL = to_bcd(tm.tm_mday);
        break;
    default:
        break; /* setting the RTC or alarms is accepted and ignored */
    }
    cb_set_carry(m, false);
    return CB_DONE;
}

/* ---- tables and boot ---- */

/* INT 15h AH=C0h: length, model FC, submodel, revision, features (cascaded PIC, RTC). */
static void install_rom_tables(Machine *m)
{
    static const uint8_t CONFIG[] = { 0x08, 0x00, ROM_MODEL_AT, 0x01, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00 };
    bios_state(m)->config_table_off = bios_rom_emit(m, CONFIG, sizeof CONFIG);
    memcpy(m->mem + real_addr(BIOS_SEG, ROM_FONT16_OFF), font_8x16, sizeof font_8x16);
    memcpy(m->mem + real_addr(BIOS_SEG, ROM_FONT8_OFF), font_8x8, sizeof font_8x8);
    m->mem[real_addr(BIOS_SEG, ROM_DUMMY_IRET_OFF)] = 0xCF;
    memcpy(m->mem + real_addr(BIOS_SEG, ROM_DATE_OFF), "01/01/94", 8);
    m->mem[real_addr(BIOS_SEG, ROM_MODEL_OFF)] = ROM_MODEL_AT;
}

/* Every vector gets an IRET; hardware IRQ vectors also EOI so a stray IRQ cannot wedge the PIC. */
static void install_default_vectors(Machine *m)
{
    for (int v = 0; v < IVT_VECTORS; v++)
        bios_set_vector(m, v, BIOS_SEG, ROM_DUMMY_IRET_OFF);
    uint16_t master = emit_eoi_iret(m, false), slave = emit_eoi_iret(m, true);
    for (int v = VEC_TIMER; v <= VEC_IRQ7; v++)
        bios_set_vector(m, v, BIOS_SEG, master);
    for (int v = VEC_IRQ8; v <= VEC_IRQ15; v++)
        bios_set_vector(m, v, BIOS_SEG, slave);
    bios_set_vector(m, VEC_FONT_HIGH, BIOS_SEG, ROM_FONT8_HIGH_OFF);
    bios_set_vector(m, VEC_FONT_GRAPHICS, BIOS_SEG, ROM_FONT8_OFF);
}

static uint32_t ticks_since_midnight(void)
{
    struct tm tm = host_now();
    uint64_t secs = (uint64_t)tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    return (uint32_t)(secs * TICKS_PER_DAY / 86400);
}

static void install_bda(Machine *m)
{
    memset(m->mem + BDA_BASE, 0, 0x100);
    bda_wr16(m, BDA_COM_PORTS, COM1_BASE);
    bda_wr16(m, BDA_COM_PORTS + 2, COM2_BASE);
    bda_wr16(m, BDA_EQUIPMENT, EQUIP_FLOPPY | EQUIP_MOUSE | EQUIP_VIDEO_COLOR80 |
                                   (m->fpu_present ? EQUIP_FPU : 0) | SERIAL_PORTS << EQUIP_SERIAL_SHIFT);
    bda_wr16(m, BDA_MEMORY_KB, CONVENTIONAL_KB);
    uint32_t ticks = ticks_since_midnight();
    bda_wr16(m, BDA_TICKS, (uint16_t)ticks);
    bda_wr16(m, BDA_TICKS + 2, (uint16_t)(ticks >> 16));
}

void bios_init(Machine *m)
{
    Bios *b = calloc(1, sizeof *b);
    if (!b)
        fatal("out of memory");
    b->rom_next = ROM_CODE_OFF;
    m->bios = b;
    install_rom_tables(m);
    install_default_vectors(m);
    install_bda(m);
    pic_init(m);
    pit_init(m);
    vga_init(m);
    kbd_init(m);
    dma_init(m);
    install_timer(m);
    callback_register(m, "int11", int11_equipment, VEC_EQUIPMENT);
    callback_register(m, "int12", int12_memory, VEC_MEMORY);
    callback_register(m, "int13", int13_disk, VEC_DISK);
    callback_register(m, "int15", int15_system, VEC_SYSTEM);
    callback_register(m, "int17", int17_printer, VEC_PRINTER);
    callback_register(m, "int1a", int1a_time, VEC_TIME);
    bios_video_init(m);
    bios_kbd_init(m);
    mouse_init(m);
    gameport_init(m);
    uart_init(m);
    audio_init(m);
    opl_init(m);
    sb_init(m);
    speaker_init(m);
}

void bios_shutdown(Machine *m)
{
    void **parts[] = { &m->bios, &m->pic, &m->pit, &m->vga, &m->kbd, &m->mouse };
    speaker_shutdown(m);
    sb_shutdown(m);
    opl_shutdown(m);
    dma_shutdown(m);
    audio_shutdown(m);
    if (m->uart)
        uart_attach(m, UART_COM1, (SerialLink){ 0 }), uart_attach(m, UART_COM2, (SerialLink){ 0 });
    free(m->uart);
    m->uart = NULL;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        free(*parts[i]);
        *parts[i] = NULL;
    }
}
