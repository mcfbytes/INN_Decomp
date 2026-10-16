/* dosrt devices: BIOS services and the emulated PC hardware (PIC, PIT, VGA, 8042 keyboard,
 * INT 33h mouse, 8250/16550 UARTs, 8237 DMA). bios_init wires them all into a Machine. */
#ifndef DOSRT_DEVICES_H
#define DOSRT_DEVICES_H

#include <stdbool.h>
#include <stdint.h>

#include "machine.h"

/* ---- boot ---- */

/* Registers every BIOS callback, device and ticker, fills the IVT and BDA and sets video
 * mode 03h. Must run before dos_init. */
void bios_init(Machine *m);
/* Frees device state allocated by bios_init (call before machine_destroy). */
void bios_shutdown(Machine *m);

/* BIOS keyboard buffer at 0040:001E, as INT 16h AH=10h/11h see it (extended codes kept). */
bool bios_kbd_peek(Machine *m, uint16_t *key);
bool bios_kbd_pop(Machine *m, uint16_t *key);
/* INT 16h AH=05h: false when the buffer is full. */
bool bios_kbd_push(Machine *m, uint16_t key);

/* ---- 8259 PIC pair (ports 20h/21h, A0h/A1h) ---- */

enum { PIC_IRQ_TIMER = 0, PIC_IRQ_KEYBOARD = 1, PIC_IRQ_CASCADE = 2, PIC_IRQ_COM2 = 3,
       PIC_IRQ_COM1 = 4, PIC_IRQ_SB = 7, PIC_IRQ_MOUSE = 12 };

void pic_init(Machine *m);
/* pic_raise_irq / pic_lower_irq / pic_acknowledge are declared in machine.h. Lines are
 * edge-triggered: raising an already-high line does nothing. */

/* ---- 8253/8254 PIT (ports 40h-43h, speaker gate at 61h) ---- */

enum { PIT_CLOCK_HZ = 1193182 };

void pit_init(Machine *m);
/* Current IRQ0 rate in Hz (18.2 by default). */
double pit_timer_hz(Machine *m);
/* True while the speaker is driven by channel 2; *hz gets its square-wave frequency. */
bool pit_speaker(Machine *m, double *hz);
/* True while port 61h enables the speaker but channel 2 is not counting (its output stays high). */
bool pit_speaker_held(Machine *m);

/* ---- 8237 DMA controller, channels 0-3 (ports 00h-0Fh, pages 87h/83h/81h/82h) ---- */

enum { DMA_CHANNELS = 4 };

typedef void (*DmaSyncFn)(Machine *m);

void dma_init(Machine *m);
void dma_shutdown(Machine *m);
/* Moves up to len bytes from memory for ch's device and returns the count; stops while masked and at
 * terminal count (*tc set), where auto-init reloads and other modes mask the channel. */
int dma_read(Machine *m, int ch, uint8_t *dst, int len, bool *tc);
/* fn runs before the guest reads or reprograms ch, so a device can first move what is due. */
void dma_set_sync(Machine *m, int ch, DmaSyncFn fn);

/* ---- VGA ---- */

enum { VGA_MAX_W = 640, VGA_MAX_H = 480 };

void vga_init(Machine *m);
/* BIOS-level mode set (03h, 0Dh, 12h, 13h); clear=false keeps video memory (AL bit 7). */
bool vga_set_mode(Machine *m, uint8_t mode, bool clear);
uint8_t vga_mode(Machine *m);
/* One DAC colour. Stored 6-bit, the same mask a 3C9h write applies. */
void vga_dac_set(Machine *m, uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void vga_dac_get(Machine *m, uint8_t index, uint8_t rgb[3]);
/* Renders the current frame as 0xAARRGGBB pixels into a VGA_MAX_W*VGA_MAX_H buffer. */
void vga_render(Machine *m, uint32_t *rgba_out, int *w, int *h);
/* Writes a PPM (P6) of the current frame; returns 0 or -1 on I/O error. */
int vga_write_ppm(Machine *m, const char *path);
/* Whole VGA state (registers, DAC, planes) for shadow verification; plain bytes, no pointers. */
size_t vga_snapshot_size(void);
void vga_snapshot_save(Machine *m, void *dst);
void vga_snapshot_load(Machine *m, const void *src);

/* ---- 8042 keyboard controller (ports 60h/64h, IRQ1) ---- */

void kbd_init(Machine *m);
/* One host key event as a set-1 scancode; extended keys pass 0xE0 | code (e.g. 0xE048 is
 * grey Up) and are sent to the guest with their E0 prefix. */
void kbd_host_key(Machine *m, uint16_t set1_scancode, bool down);

/* ---- INT 33h mouse driver ---- */

void mouse_init(Machine *m);
void gameport_init(Machine *m);
/* Absolute position in the pixels of the frame vga_render produces (w x h of that frame). */
void mouse_host_motion(Machine *m, int x, int y, int frame_w, int frame_h);
/* Button 0 left, 1 right, 2 middle. */
void mouse_host_button(Machine *m, int button, bool down);
/* The driver's show/hide state, for a frontend-drawn cursor overlay. */
bool mouse_cursor_visible(Machine *m);
/* Pointer position in frame pixels, for the overlay. */
void mouse_cursor_pos(Machine *m, int frame_w, int frame_h, int *x, int *y);

/* ---- 8250/16550 UART (COM1 3F8h IRQ4, COM2 2F8h IRQ3) ---- */

/* A byte pipe standing in for the modem on the far side of the UART. Returns of send/recv
 * are byte counts (0 when nothing moves, -1 on a broken link). */
typedef struct SerialLink {
    void *ctx;
    int (*send)(void *ctx, const uint8_t *buf, int len);
    int (*recv)(void *ctx, uint8_t *buf, int cap);
    bool (*connected)(void *ctx); /* drives DCD/DSR/CTS */
    void (*close)(void *ctx);
} SerialLink;

enum { UART_COM1 = 1, UART_COM2 = 2 };

void uart_init(Machine *m);
/* Takes ownership of link (closed by bios_shutdown or the next attach). com is 1 or 2. */
int uart_attach(Machine *m, int com, SerialLink link);
/* The link attached to COM1/COM2 (NULL for a bad port number). */
SerialLink *uart_link(Machine *m, int com);
/* Raw TCP client link to e.g. innkeeperd's line port; 0 on success, -1 on failure. */
int uart_attach_tcp(Machine *m, int com, const char *host, int port);
/* Browser builds: WebSocket link (ws:// or wss:// URL, binary frames of raw bytes); the native
 * build returns -1. Connects in the background like the TCP link. */
int uart_attach_ws(Machine *m, int com, const char *url);
/* In-memory link for tests: bytes sent by the guest are readable with uart_test_take,
 * bytes given to uart_test_give arrive at the guest. */
int uart_attach_pipe(Machine *m, int com);
int uart_test_give(Machine *m, int com, const uint8_t *buf, int len);
int uart_test_take(Machine *m, int com, uint8_t *buf, int cap);

#endif
