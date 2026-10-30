/* SDL2 frontend: input into the machine, frames and sound out. Per-frame work is in frame_step so
 * the browser build can drive it from emscripten_set_main_loop; --headless needs no display. */
#include <SDL.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "audio_sdl.h"
#include "lift.h"
#include "devices.h"
#include "host_input.h"
#include "audio.h"
#include "frame_clock.h"
#include "input_script.h"
#include "exec_stub.h"
#include "host_rules.h"
#if INN_NATIVE_HW
#include "host_link.h"
#endif
#include "machine.h"
#if __has_include("dos.h")
#include "dos.h"
#include "emsxms.h"
#define HAVE_DOS 1
#endif
#ifdef INN_NATIVE
#include "host_input.h"
#include "host_platform.h"
#include "host_video.h"
#include "lift_internal.h"
#include "native.h"
#include "native_exec.h"
#endif

enum {
    WINDOW_W = 960, WINDOW_H = 720, /* 4:3, so 200-line modes get their tall pixels */
    HEADLESS_CHUNK_US = 10000,
    FRAME_PERIOD_US = 16667,
    NESTED_AHEAD_US = 50000,   /* the frame clock's largest burst (MAX_FRAME_US): no wait below it */
    NESTED_CHECK_US = 1000,
    LONG_FRAME_US = 100000,    /* --perf reports frames longer than this */    /* emulated time between wall-clock checks in a nested wait */
    NESTED_PRESENT_US = 16667,
    MIXED_AHEAD_US = 10000,    /* a game on the emulator: sleep once emulated time is this far ahead */
    MIXED_BEHIND_US = 50000,   /* and drop the lost time once it is this far behind */
    DEFAULT_RUN_MS = 10000,
    ARGS_MAX = 128,
};

typedef struct Options {
    int trace;
    uint64_t ips;
    const char *com1;
    bool headless, perf, audiotap;
    uint64_t run_ms;
    const char *screenshot;
    const char *script;
    const char *sync_dir;
    const char *wav;
    unsigned conv_kb;
    unsigned ems_kb, xms_kb; /* 0: none, as without these options */
    bool no_fpu, no_idle_skip;
    const char *host_rules;
    const char *c_dir, *program;
    char args[ARGS_MAX];
} Options;

typedef struct Frontend {
    Machine *m;
    const Options *o;
    InputScript *script;        /* headless runs */
    uint64_t end_us;            /* headless runs end at this emulated time (--run-ms) */
    uint64_t script_due_us;     /* a nested wait applies the script once past the current chunk */
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t frame[VGA_MAX_W * VGA_MAX_H];
    int frame_w, frame_h;
#ifdef __EMSCRIPTEN__
    uint32_t sent[VGA_MAX_W * VGA_MAX_H]; /* the frame the page's GPU filter last received */
    int sent_w, sent_h;
#endif
    FrameClock clock;
    uint64_t step_wall_us, step_virtual_us; /* at the start of the current frame */
    uint64_t last_present_us;
    uint64_t nested_checked_us;
    uint64_t mixed_anchor_wall_us, mixed_anchor_virtual_us; /* a game on the emulator: time that corresponds */
    uint64_t slept_us, yields; /* in the current frame, for --perf long-frame reports */
    bool perf; /* emulated time of the last wall-clock check in a nested wait */
    /* Native runs do not go through frame_step. --perf is accounted at each platform sleep:
     * host time since the previous sleep returned is the busy slice, and a 5 s wall window
     * prints the same line frame_clock_run prints. */
    uint64_t perf_mark_us, perf_mark_virtual_us;
    uint64_t perf_window_us, perf_virtual_us, perf_instructions, perf_busy_us, perf_longest_us, perf_skipped_us;
    bool quit;
} Frontend;

/* ---- input mapping ---- */

typedef struct KeyMap { SDL_Scancode sdl; uint16_t set1; } KeyMap;

static const KeyMap KEYMAP[] = {
    { SDL_SCANCODE_ESCAPE, 0x01 }, { SDL_SCANCODE_1, 0x02 }, { SDL_SCANCODE_2, 0x03 },
    { SDL_SCANCODE_3, 0x04 }, { SDL_SCANCODE_4, 0x05 }, { SDL_SCANCODE_5, 0x06 },
    { SDL_SCANCODE_6, 0x07 }, { SDL_SCANCODE_7, 0x08 }, { SDL_SCANCODE_8, 0x09 },
    { SDL_SCANCODE_9, 0x0A }, { SDL_SCANCODE_0, 0x0B }, { SDL_SCANCODE_MINUS, 0x0C },
    { SDL_SCANCODE_EQUALS, 0x0D }, { SDL_SCANCODE_BACKSPACE, 0x0E }, { SDL_SCANCODE_TAB, 0x0F },
    { SDL_SCANCODE_Q, 0x10 }, { SDL_SCANCODE_W, 0x11 }, { SDL_SCANCODE_E, 0x12 },
    { SDL_SCANCODE_R, 0x13 }, { SDL_SCANCODE_T, 0x14 }, { SDL_SCANCODE_Y, 0x15 },
    { SDL_SCANCODE_U, 0x16 }, { SDL_SCANCODE_I, 0x17 }, { SDL_SCANCODE_O, 0x18 },
    { SDL_SCANCODE_P, 0x19 }, { SDL_SCANCODE_LEFTBRACKET, 0x1A }, { SDL_SCANCODE_RIGHTBRACKET, 0x1B },
    { SDL_SCANCODE_RETURN, 0x1C }, { SDL_SCANCODE_LCTRL, 0x1D }, { SDL_SCANCODE_A, 0x1E },
    { SDL_SCANCODE_S, 0x1F }, { SDL_SCANCODE_D, 0x20 }, { SDL_SCANCODE_F, 0x21 },
    { SDL_SCANCODE_G, 0x22 }, { SDL_SCANCODE_H, 0x23 }, { SDL_SCANCODE_J, 0x24 },
    { SDL_SCANCODE_K, 0x25 }, { SDL_SCANCODE_L, 0x26 }, { SDL_SCANCODE_SEMICOLON, 0x27 },
    { SDL_SCANCODE_APOSTROPHE, 0x28 }, { SDL_SCANCODE_GRAVE, 0x29 }, { SDL_SCANCODE_LSHIFT, 0x2A },
    { SDL_SCANCODE_BACKSLASH, 0x2B }, { SDL_SCANCODE_Z, 0x2C }, { SDL_SCANCODE_X, 0x2D },
    { SDL_SCANCODE_C, 0x2E }, { SDL_SCANCODE_V, 0x2F }, { SDL_SCANCODE_B, 0x30 },
    { SDL_SCANCODE_N, 0x31 }, { SDL_SCANCODE_M, 0x32 }, { SDL_SCANCODE_COMMA, 0x33 },
    { SDL_SCANCODE_PERIOD, 0x34 }, { SDL_SCANCODE_SLASH, 0x35 }, { SDL_SCANCODE_RSHIFT, 0x36 },
    { SDL_SCANCODE_KP_MULTIPLY, 0x37 }, { SDL_SCANCODE_LALT, 0x38 }, { SDL_SCANCODE_SPACE, 0x39 },
    { SDL_SCANCODE_CAPSLOCK, 0x3A }, { SDL_SCANCODE_F1, 0x3B }, { SDL_SCANCODE_F2, 0x3C },
    { SDL_SCANCODE_F3, 0x3D }, { SDL_SCANCODE_F4, 0x3E }, { SDL_SCANCODE_F5, 0x3F },
    { SDL_SCANCODE_F6, 0x40 }, { SDL_SCANCODE_F7, 0x41 }, { SDL_SCANCODE_F8, 0x42 },
    { SDL_SCANCODE_F9, 0x43 }, { SDL_SCANCODE_F10, 0x44 }, { SDL_SCANCODE_NUMLOCKCLEAR, 0x45 },
    { SDL_SCANCODE_SCROLLLOCK, 0x46 }, { SDL_SCANCODE_KP_7, 0x47 }, { SDL_SCANCODE_KP_8, 0x48 },
    { SDL_SCANCODE_KP_9, 0x49 }, { SDL_SCANCODE_KP_MINUS, 0x4A }, { SDL_SCANCODE_KP_4, 0x4B },
    { SDL_SCANCODE_KP_5, 0x4C }, { SDL_SCANCODE_KP_6, 0x4D }, { SDL_SCANCODE_KP_PLUS, 0x4E },
    { SDL_SCANCODE_KP_1, 0x4F }, { SDL_SCANCODE_KP_2, 0x50 }, { SDL_SCANCODE_KP_3, 0x51 },
    { SDL_SCANCODE_KP_0, 0x52 }, { SDL_SCANCODE_KP_PERIOD, 0x53 }, { SDL_SCANCODE_F11, 0x57 },
    { SDL_SCANCODE_F12, 0x58 },
    /* E0-prefixed keys of the 101-key layout */
    { SDL_SCANCODE_KP_ENTER, 0xE01C }, { SDL_SCANCODE_RCTRL, 0xE01D }, { SDL_SCANCODE_KP_DIVIDE, 0xE035 },
    { SDL_SCANCODE_RALT, 0xE038 }, { SDL_SCANCODE_HOME, 0xE047 }, { SDL_SCANCODE_UP, 0xE048 },
    { SDL_SCANCODE_PAGEUP, 0xE049 }, { SDL_SCANCODE_LEFT, 0xE04B }, { SDL_SCANCODE_RIGHT, 0xE04D },
    { SDL_SCANCODE_END, 0xE04F }, { SDL_SCANCODE_DOWN, 0xE050 }, { SDL_SCANCODE_PAGEDOWN, 0xE051 },
    { SDL_SCANCODE_INSERT, 0xE052 }, { SDL_SCANCODE_DELETE, 0xE053 },
};

static uint16_t to_set1(SDL_Scancode sc)
{
    for (size_t i = 0; i < sizeof KEYMAP / sizeof KEYMAP[0]; i++)
        if (KEYMAP[i].sdl == sc)
            return KEYMAP[i].set1;
    return 0;
}

static int to_guest_button(uint8_t sdl_button)
{
    switch (sdl_button) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    default: return -1;
    }
}

/* Window coordinates are logical (WINDOW_W x WINDOW_H) thanks to SDL_RenderSetLogicalSize. */
static void mouse_to_guest(Frontend *f, int wx, int wy)
{
    int x = wx * f->frame_w / WINDOW_W, y = wy * f->frame_h / WINDOW_H;
    host_input_mouse_motion(f->m, x, y, f->frame_w, f->frame_h);
}

static void handle_event(Frontend *f, const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: f->quit = true; break;
    case SDL_KEYDOWN: case SDL_KEYUP: {
        uint16_t sc = to_set1(e->key.keysym.scancode);
        if (sc)
            host_input_key(f->m, sc, e->type == SDL_KEYDOWN);
        break;
    }
    case SDL_MOUSEMOTION: mouse_to_guest(f, e->motion.x, e->motion.y); break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        mouse_to_guest(f, e->button.x, e->button.y);
        host_input_mouse_button(f->m, to_guest_button(e->button.button), e->type == SDL_MOUSEBUTTONDOWN);
        break;
    default: break;
    }
}

/* ---- drawing ---- */

/* A 7x11 arrow for when the guest asks the driver to show its cursor (1 white, 2 black). */
static const char *const ARROW[] = {
    "2", "22", "212", "2112", "21112", "211112", "2111112", "2111222", "21212", "22 212", "    22",
};

static void draw_cursor_overlay(Frontend *f)
{
    if (!mouse_cursor_visible(f->m))
        return;
    int cx, cy;
    mouse_cursor_pos(f->m, f->frame_w, f->frame_h, &cx, &cy);
    for (size_t y = 0; y < sizeof ARROW / sizeof ARROW[0]; y++)
        for (size_t x = 0; ARROW[y][x]; x++) {
            int px = cx + (int)x, py = cy + (int)y;
            if (ARROW[y][x] == ' ' || px >= f->frame_w || py >= f->frame_h)
                continue;
            f->frame[py * f->frame_w + px] = ARROW[y][x] == '1' ? 0xFFFFFFFFu : 0xFF000000u;
        }
}

#ifdef __EMSCRIPTEN__
/* The page's display filters (runtime/web/filters.js, docs/web.md "Display filters"). wants() is 0 with Sharp or
 * Smooth, when SDL draws the frame as before; 1 when a GPU filter is on and wants the frame only if it changed;
 * 2 when it wants the current frame whatever (just switched on, or the context was rebuilt). */
EM_JS(int, inn_gpu_wants, (void), { return (typeof Filters !== 'undefined') ? Filters.wants() : 0; });
EM_JS(void, inn_gpu_frame, (const void *pixels, int w, int h), { Filters.frame(HEAPU8, pixels, w, h); });

/* Hands the frame to the filter. Returns false when no filter is on and SDL should draw it. */
static bool gpu_present(Frontend *f)
{
    int want = inn_gpu_wants();
    if (!want) {
        f->sent_w = 0; /* the filter has not seen the frames since: send the next one whole */
        return false;
    }
    size_t bytes = (size_t)f->frame_w * (size_t)f->frame_h * sizeof f->frame[0];
    if (want == 2 || f->sent_w != f->frame_w || f->sent_h != f->frame_h || memcmp(f->sent, f->frame, bytes) != 0) {
        memcpy(f->sent, f->frame, bytes);
        f->sent_w = f->frame_w, f->sent_h = f->frame_h;
        inn_gpu_frame(f->frame, f->frame_w, f->frame_h);
    }
    return true;
}
#endif

/* f->frame holds the rendered frame. */
static void show_frame(Frontend *f)
{
    draw_cursor_overlay(f);
#ifdef __EMSCRIPTEN__
    if (gpu_present(f))
        return;
#endif
    SDL_Rect src = { 0, 0, f->frame_w, f->frame_h };
    SDL_UpdateTexture(f->texture, &src, f->frame, f->frame_w * (int)sizeof f->frame[0]);
    SDL_RenderClear(f->renderer);
    SDL_RenderCopy(f->renderer, f->texture, &src, NULL);
    SDL_RenderPresent(f->renderer);
}

static void present(Frontend *f)
{
    f->last_present_us = frame_clock_now_us();
    vga_render(f->m, f->frame, &f->frame_w, &f->frame_h);
    show_frame(f);
}

/* ---- main loop ---- */

static Frontend *g_frontend; /* emscripten_set_main_loop takes no context argument */

static void host_sleep_us(uint64_t us)
{
    if (g_frontend) {
        uint64_t start = frame_clock_now_us();
        g_frontend->yields++;
#ifdef INN_ASYNCIFY
        emscripten_sleep((unsigned)(us / 1000));
#elif !defined(__EMSCRIPTEN__)
        SDL_Delay((uint32_t)(us / 1000));
#endif
        g_frontend->slept_us += frame_clock_now_us() - start;
        return;
    }
#ifdef INN_ASYNCIFY
    emscripten_sleep((unsigned)(us / 1000));
#elif !defined(__EMSCRIPTEN__)
    SDL_Delay((uint32_t)(us / 1000));
#else
    (void)us; /* without Asyncify the browser cannot be yielded to from inside a frame */
#endif
}

/* Lifted code waiting inside one frame (a dial, kWait): keep emulated time with the wall clock,
 * show frames and take input, as the frame loop would. */
void frontend_nested_yield(Machine *m)
{
    Frontend *f = g_frontend;
    if (m->virtual_us - f->nested_checked_us < NESTED_CHECK_US)
        return;
    f->nested_checked_us = m->virtual_us;
    uint64_t now = frame_clock_now_us();
    uint64_t emulated = m->virtual_us - f->step_virtual_us, wall = now - f->step_wall_us;
    /* A frame normally runs its emulated time in a burst; only a wait longer than that is paced. */
    if (emulated > wall + NESTED_AHEAD_US)
        host_sleep_us(emulated - wall - NESTED_AHEAD_US);
    if (wall < NESTED_PRESENT_US || now - f->last_present_us < NESTED_PRESENT_US)
        return;
    SDL_Event e;
    while (SDL_PollEvent(&e))
        handle_event(f, &e);
    frontend_audio_pump(m);
    present(f);
    host_sleep_us(0); /* a poll loop may take no emulated time; the browser still needs its turn */
}

static void frame_step(void)
{
    Frontend *f = g_frontend;
    SDL_Event e;
    while (SDL_PollEvent(&e))
        handle_event(f, &e);
    f->step_wall_us = frame_clock_now_us();
    f->step_virtual_us = f->m->virtual_us;
    f->slept_us = f->yields = 0;
    frame_clock_run(&f->clock, f->m);
    uint64_t wall = frame_clock_now_us() - f->step_wall_us;
    if (f->perf && wall > LONG_FRAME_US)
        log_msg("perf: long frame %llu ms: emulated %llu ms, slept %llu ms in %llu yields",
                (unsigned long long)wall / 1000, (unsigned long long)(f->m->virtual_us - f->step_virtual_us) / 1000,
                (unsigned long long)f->slept_us / 1000, (unsigned long long)f->yields);
    frontend_audio_pump(f->m);
    present(f);
    if (f->m->exit_requested)
        f->quit = true;
}

#ifdef __EMSCRIPTEN__
static void report_exit(Frontend *f)
{
    EM_ASM({ if (Module.onGuestExit) Module.onGuestExit($0); }, f->m->exit_code);
}

#ifndef INN_ASYNCIFY
static void main_loop_step(void)
{
    frame_step();
    if (g_frontend->quit) {
        emscripten_cancel_main_loop();
        report_exit(g_frontend);
    }
}
#endif
#endif

#ifdef __EMSCRIPTEN__
enum { WINDOW_FLAGS = 0 }; /* the page sizes the canvas */
#else
enum { WINDOW_FLAGS = SDL_WINDOW_RESIZABLE };
#endif

static int open_window(Frontend *f)
{
#ifdef __EMSCRIPTEN__
    /* Keys reach the guest only while the canvas has focus, so the page's own fields still type. */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
    /* Our loop yields to the browser itself; SDL's own sleep in SwapWindow is not instrumented. */
    SDL_SetHint("SDL_EMSCRIPTEN_ASYNCIFY", "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
#ifdef __EMSCRIPTEN__
    const char *title = "Online world"; /* the page title; the page owns any site name */
    SDL_ShowCursor(SDL_DISABLE);        /* only the game's own cursor shows over the canvas */
#else
    const char *title = "ImagiNation Network";
#endif
    f->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 WINDOW_W, WINDOW_H, WINDOW_FLAGS);
    if (!f->window)
        return -1;
    f->renderer = SDL_CreateRenderer(f->window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!f->renderer)
        return -1;
    SDL_RenderSetLogicalSize(f->renderer, WINDOW_W, WINDOW_H);
    f->texture = SDL_CreateTexture(f->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                   VGA_MAX_W, VGA_MAX_H);
    return f->texture ? 0 : -1;
}

static void close_window(Frontend *f)
{
    if (f->texture)
        SDL_DestroyTexture(f->texture);
    if (f->renderer)
        SDL_DestroyRenderer(f->renderer);
    if (f->window)
        SDL_DestroyWindow(f->window);
    SDL_Quit();
}

static int start_windowed(Frontend *f, const Options *o)
{
    if (open_window(f) != 0) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        close_window(f);
        return -1;
    }
    f->frame_w = 320, f->frame_h = 200;
    frontend_audio_open(f->m, true, o->wav);
    if (o->audiotap)
        audio_tap_enable(f->m, true);
    frame_clock_start(&f->clock, f->m, o->perf);
    f->perf = o->perf;
    g_frontend = f;
    return 0;
}

static int run_windowed(Frontend *f)
{
    lift_set_nested_yield(f->m, frontend_nested_yield);
#if defined(INN_ASYNCIFY)
    /* A plain loop rather than emscripten_set_main_loop: lifted code may sleep inside a frame. */
    while (!f->quit) {
        uint64_t start = frame_clock_now_us();
        frame_step();
        uint64_t spent = frame_clock_now_us() - start;
        host_sleep_us(spent < FRAME_PERIOD_US ? FRAME_PERIOD_US - spent : 0);
    }
    report_exit(f);
#elif defined(__EMSCRIPTEN__)
    emscripten_set_main_loop(main_loop_step, 0, 1);
#else
    while (!f->quit)
        frame_step();
    frontend_audio_close(f->m);
    close_window(f);
#endif
    return 0;
}

static int finish_run(Frontend *f, int rc);

/* Applies the input script due by now and ends the run once --run-ms has passed, from wherever the
 * machine waits: a session that never returns from C still ends with its summary. */
static void headless_wait_point(Machine *m)
{
    Frontend *f = g_frontend;
    if (m->virtual_us >= f->end_us)
        exit(finish_run(f, 0));
    if (f->script && input_script_apply(f->script, m) != 0)
        exit(finish_run(f, 1));
    frontend_audio_pump(m);
}

/* An input script's "wait" holds the machine still; the server's bytes queue in the link meanwhile. */
static void hold_link(void *ctx)
{
#if INN_NATIVE_HW
    host_link_drain(ctx);
#else
    serial_link_drain(uart_link(ctx, UART_COM1));
#endif
}

/* A nested wait that outlasts the current chunk (a native start running inside the hybrid build)
 * gets the script at the chunk rate of the headless loop. */
static void headless_nested_yield(Machine *m)
{
    Frontend *f = g_frontend;
    if (m->virtual_us < f->script_due_us && m->virtual_us < f->end_us)
        return;
    f->script_due_us = m->virtual_us + HEADLESS_CHUNK_US;
    headless_wait_point(m);
}

static int start_headless(Frontend *f, const Options *o)
{
    f->end_us = f->m->virtual_us + o->run_ms * 1000;
    if (o->script && !(f->script = input_script_load(o->script))) {
        fprintf(stderr, "cannot load script %s\n", o->script);
        return -1;
    }
    if (f->script)
        input_script_set_sync(f->script, o->sync_dir, hold_link, f->m);
    if (o->wav && frontend_audio_open(f->m, false, o->wav) != 0)
        return -1;
    if (o->audiotap)
        audio_tap_enable(f->m, true);
    g_frontend = f;
    return 0;
}

static int run_headless(Frontend *f)
{
    lift_set_nested_yield(f->m, headless_nested_yield);
    while (f->m->virtual_us < f->end_us && !f->m->exit_requested) {
        if (f->script && input_script_apply(f->script, f->m) != 0)
            return -1;
        f->script_due_us = f->m->virtual_us + HEADLESS_CHUNK_US;
        machine_run_for(f->m, HEADLESS_CHUNK_US);
        frontend_audio_pump(f->m);
    }
    return 0;
}

#ifdef INN_NATIVE
/* ---- native boot (docs/native.md section 4): the program drives ---- */

enum { NATIVE_NO_START = -1 };

static uint64_t platform_now_us(void *ctx)
{
    return frame_clock_now_us();
}

#ifdef __EMSCRIPTEN__
/* A zero-delay turn of the browser's event loop: a MessageChannel message avoids the 4 ms clamp of
 * nested setTimeout. The whole C stack is suspended meanwhile (JSPI). */
EM_ASYNC_JS(void, browser_turn, (void), {
    await new Promise(resolve => {
        const channel = new MessageChannel();
        channel.port1.onmessage = () => resolve();
        channel.port2.postMessage(0);
    });
});
#endif

/* One busy slice of a native (or mixed-mode) run ended because the platform is about to sleep. */
static void perf_before_sleep(Frontend *f)
{
    if (!f->perf || !f->m)
        return;
    uint64_t now = frame_clock_now_us();
    if (!f->perf_mark_us) {
        f->perf_mark_us = f->perf_window_us = now;
        f->perf_mark_virtual_us = f->perf_virtual_us = f->m->virtual_us;
        f->perf_instructions = f->m->cpu.instructions;
        f->perf_skipped_us = f->m->idle.skipped_us;
        return;
    }
    uint64_t busy = now - f->perf_mark_us;
    uint64_t emulated = f->m->virtual_us - f->perf_mark_virtual_us;
    f->perf_busy_us += busy;
    if (busy > f->perf_longest_us)
        f->perf_longest_us = busy;
    if (busy > LONG_FRAME_US)
        log_msg("perf: long frame %llu ms: emulated %llu ms, slept 0 ms in 1 yields",
                (unsigned long long)(busy / 1000), (unsigned long long)(emulated / 1000));
    if (now - f->perf_window_us < 5000000)
        return;
    uint64_t wall = now - f->perf_window_us;
    uint64_t advanced = f->m->virtual_us - f->perf_virtual_us;
    uint64_t instructions = f->m->cpu.instructions - f->perf_instructions;
    double busy_d = f->perf_busy_us ? (double)f->perf_busy_us : 1.0;
    AudioStats audio = audio_take_stats(f->m);
    double idle = advanced ? 100.0 * (double)(f->m->idle.skipped_us - f->perf_skipped_us) / (double)advanced : 0.0;
    log_msg("perf: speed %.3fx real, host busy %.1f%%, executed %.2f MIPS, capacity %.1f MIPS "
            "(guest clock %.0f MIPS), dropped %.0f ms, longest frame %.0f ms, "
            "audio underruns %u, resyncs %u, discarded %.0f ms, idle %.0f%%",
            (double)advanced / (double)wall, 100.0 * (double)f->perf_busy_us / (double)wall,
            (double)instructions / (double)wall, (double)instructions / busy_d,
            (double)f->m->ips / 1e6, wall > advanced ? (wall - advanced) / 1000.0 : 0.0,
            f->perf_longest_us / 1000.0, audio.underruns, audio.resyncs,
            audio_rate(f->m) ? audio.dropped_frames * 1000.0 / audio_rate(f->m) : 0.0, idle > 100.0 ? 100.0 : idle);
    f->perf_window_us = now;
    f->perf_skipped_us = f->m->idle.skipped_us;
    f->perf_virtual_us = f->m->virtual_us;
    f->perf_instructions = f->m->cpu.instructions;
    f->perf_busy_us = 0;
    f->perf_longest_us = 0;
}

static void platform_sleep_us(void *ctx, uint64_t us)
{
    Frontend *f = ctx;
    f->yields++;
    perf_before_sleep(f);
#ifdef __EMSCRIPTEN__
    if (us < 1000)
        browser_turn();
    else
        emscripten_sleep((unsigned)(us / 1000));
#else
    SDL_Delay((uint32_t)(us / 1000));
#endif
    /* The sleep itself is pacing, not work. The next slice starts when the browser resumes. */
    if (f->perf && f->m) {
        f->perf_mark_us = frame_clock_now_us();
        f->perf_mark_virtual_us = f->m->virtual_us;
    }
}

static void end_windowed(Frontend *f)
{
#ifdef __EMSCRIPTEN__
    report_exit(f);
#else
    frontend_audio_close(f->m);
    close_window(f);
#endif
}

static void platform_poll_events(void *ctx, Machine *m);

/* A game runs on the emulator inside native_exec (docs/native.md section 11): native_exec steps the machine
 * and calls this after every step, which does what frame_step does per frame: keep emulated time with the
 * wall clock, take input, pump audio, draw the VGA device's frame. */
static void mixed_yield(Machine *m)
{
    Frontend *f = g_frontend;
    if (m->virtual_us - f->nested_checked_us < NESTED_CHECK_US)
        return;
    f->nested_checked_us = m->virtual_us;
    uint64_t now = frame_clock_now_us();
    int64_t ahead = (int64_t)(m->virtual_us - f->mixed_anchor_virtual_us) -
                    (int64_t)(now - f->mixed_anchor_wall_us);
    if (ahead > MIXED_AHEAD_US) {
        platform_sleep_us(f, (uint64_t)ahead);
        now = frame_clock_now_us();
    } else if (ahead < -(int64_t)MIXED_BEHIND_US) {
        f->mixed_anchor_wall_us = now, f->mixed_anchor_virtual_us = m->virtual_us;
    }
    if (now - f->last_present_us < NESTED_PRESENT_US)
        return;
    platform_poll_events(f, m);
    frontend_audio_pump(m);
    present(f);
    platform_sleep_us(f, 0); /* a poll loop may take no emulated time; the browser still needs its turn */
}

static void platform_poll_events(void *ctx, Machine *m)
{
    Frontend *f = ctx;
    SDL_Event e;
    while (SDL_PollEvent(&e))
        handle_event(f, &e);
    if (f->quit) {
        end_windowed(f);
        exit(finish_run(f, 0));
    }
}

static void platform_present(void *ctx, const uint32_t *argb, int w, int h)
{
    Frontend *f = ctx;
    f->last_present_us = frame_clock_now_us();
    f->frame_w = w, f->frame_h = h;
    memcpy(f->frame, argb, (size_t)w * (size_t)h * sizeof f->frame[0]);
    show_frame(f);
}

static void platform_audio_pump(void *ctx, Machine *m)
{
    frontend_audio_pump(m);
}

static bool platform_quit_requested(void *ctx)
{
    return ((Frontend *)ctx)->quit;
}

/* The top program was loaded before native mode was on, so the native-only entries of its module are
 * not armed yet: arm them now, and those of every module loaded later. */
enum { PSP_PARAS = 0x10, MCB_SIZE_WORD = 3 };

static void arm_native_control(Machine *m)
{
    lift_state(m)->native_control = true;
    uint16_t load_seg = 0;
    const char *module = lift_module_owning(m, m->cpu.seg[S_CS], &load_seg);
    if (!module)
        return;
    char name[sizeof lift_state(m)->modules[0].name];
    snprintf(name, sizeof name, "%s", module);
    uint16_t paras = mem_rd16(m, real_addr((uint16_t)(load_seg - PSP_PARAS - 1), MCB_SIZE_WORD));
    lift_module_loaded(m, name, load_seg, (uint32_t)(paras - PSP_PARAS) * 16);
}

static bool run_to_native_start(Machine *m)
{
    arm_native_control(m);
    return native_unpack_to_start(m);
}

/* Runs the program natively; without a native start it runs on the emulator as in the hybrid build. */
static int run_native(Frontend *f)
{
    static HostPlatform platform;
    platform = (HostPlatform){ .ctx = f, .now_us = platform_now_us, .sleep_us = platform_sleep_us,
                               .poll_events = platform_poll_events, .present = platform_present,
                               .audio_pump = platform_audio_pump, .quit_requested = platform_quit_requested };
    Machine *m = f->m;
    uint16_t cs = m->cpu.seg[S_CS], ip = (uint16_t)m->cpu.eip;
    if (f->o->headless) {
        native_set_platform(m, NULL);
        native_set_headless_hook(m, headless_wait_point);
    } else {
        g_frontend = f;
        native_set_platform(m, &platform);
    }
    if (run_to_native_start(m)) {
        native_set_active(m, true);
        f->mixed_anchor_wall_us = frame_clock_now_us(), f->mixed_anchor_virtual_us = m->virtual_us;
        lift_set_nested_yield(m, f->o->headless ? headless_nested_yield : mixed_yield);
        native_run_top(m);
        native_set_active(m, false);
        if (!f->o->headless)
            end_windowed(f);
        return 0;
    }
    log_msg("native: %s has no native start at %04X:%04X; it runs on the emulator", f->o->program, cs, ip);
    native_set_platform(m, NULL);
    return f->o->headless ? run_headless(f) : run_windowed(f);
}
#endif

/* ---- command line ---- */

static void usage(void)
{
    fprintf(stderr, "usage: inn [--trace N] [--ips N] [--com1 host:port|ws://host:port] [--perf] [--audiotap] [--wav out.wav] [--conv-kb N] [--ems-kb N] [--xms-kb N] [--no-fpu] [--no-idle-skip] [--tsn-host rules.txt] "
                    "[--headless --run-ms N --screenshot out.ppm --script input.txt [--sync-dir dir]] <c-drive-dir> <program> [args]\n");
}

static int parse_args(int argc, char **argv, Options *o)
{
    *o = (Options){ .run_ms = DEFAULT_RUN_MS };
    int i = 1;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--headless")) { o->headless = true; continue; }
        if (!strcmp(a, "--perf")) { o->perf = true; continue; }
        if (!strcmp(a, "--audiotap")) { o->audiotap = true; continue; }
        if (!strcmp(a, "--no-fpu")) { o->no_fpu = true; continue; }
        if (!strcmp(a, "--no-idle-skip")) { o->no_idle_skip = true; continue; }
        if (!next)
            return -1;
        if (!strcmp(a, "--trace")) o->trace = atoi(next);
        else if (!strcmp(a, "--ips")) o->ips = strtoull(next, NULL, 10);
        else if (!strcmp(a, "--com1")) o->com1 = next;
        else if (!strcmp(a, "--run-ms")) o->run_ms = strtoull(next, NULL, 10);
        else if (!strcmp(a, "--screenshot")) o->screenshot = next;
        else if (!strcmp(a, "--script")) o->script = next;
        else if (!strcmp(a, "--sync-dir")) o->sync_dir = next;
        else if (!strcmp(a, "--wav")) o->wav = next;
        else if (!strcmp(a, "--tsn-host")) o->host_rules = next;
        else if (!strcmp(a, "--conv-kb")) o->conv_kb = (unsigned)strtoul(next, NULL, 10);
        else if (!strcmp(a, "--ems-kb")) o->ems_kb = (unsigned)strtoul(next, NULL, 10);
        else if (!strcmp(a, "--xms-kb")) o->xms_kb = (unsigned)strtoul(next, NULL, 10);
        else return -1;
        i++;
    }
    if (argc - i < 2)
        return -1;
    o->c_dir = argv[i];
    o->program = argv[i + 1];
    for (int k = i + 2; k < argc; k++) {
        if (o->args[0])
            strncat(o->args, " ", sizeof o->args - strlen(o->args) - 1);
        strncat(o->args, argv[k], sizeof o->args - strlen(o->args) - 1);
    }
    return 0;
}

static int attach_com1(Machine *m, const char *spec)
{
#if INN_NATIVE_HW
    /* The converted drivers read the link through the host API, not the UART. */
    return host_link_open(m, spec);
#endif
    if (!strncmp(spec, "ws://", 5) || !strncmp(spec, "wss://", 6))
        return uart_attach_ws(m, UART_COM1, spec);
    char host[256];
    const char *colon = strrchr(spec, ':');
    if (!colon || (size_t)(colon - spec) >= sizeof host)
        return -1;
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = 0;
    return uart_attach_tcp(m, UART_COM1, host, atoi(colon + 1));
}

static HostRules *host_rules;

static int boot(Machine *m, const Options *o)
{
    bios_init(m);
    if (o->host_rules) {
        exec_stub_install(m, NULL, NULL);
        if (!(host_rules = host_rules_load(m, o->host_rules))) {
            fprintf(stderr, "cannot load TSN host rules %s\n", o->host_rules);
            return -1;
        }
        exec_stub_set_send_hook(m, host_rules_on_send, host_rules);
        exec_stub_set_poll_hook(m, host_rules_poll, host_rules);
    }
    if (o->com1 && attach_com1(m, o->com1) != 0) {
        fprintf(stderr, "cannot connect COM1 to %s\n", o->com1);
        return -1;
    }
#ifdef HAVE_DOS
    char path[256];
    snprintf(path, sizeof path, "C:\\%s", o->program);
    dos_init(m, o->c_dir);
    if (o->conv_kb)
        dos_limit_memory(m, o->conv_kb);
#ifdef INN_NATIVE
    if (o->ems_kb || o->xms_kb) /* the native client answers its EMS and XMS probes with "none" */
        log_msg("native build: --ems-kb and --xms-kb are ignored");
#else
    if (!emsxms_init(m, o->ems_kb, o->xms_kb)) {
        fprintf(stderr, "--ems-kb %u --xms-kb %u do not fit in 16 MB\n", o->ems_kb, o->xms_kb);
        return -1;
    }
#endif
    /* Start in the program's directory, as if launched from a prompt there. */
    char cwd[256];
    snprintf(cwd, sizeof cwd, "\\%s", o->program);
    char *slash = strrchr(cwd, '\\');
    slash[slash == cwd ? 1 : 0] = 0;
    dos_set_cwd(m, cwd);
    if (!dos_start_program(m, path, o->args)) {
        fprintf(stderr, "cannot start %s\n", path);
        return -1;
    }
#else
    fprintf(stderr, "built without dos.h: BIOS only\n");
#endif
    return 0;
}

/* The end of every run, also from inside a wait that never returns: script, audio and screenshot of a
 * headless run, then the machine, whose destruction prints the verification summary and writes
 * coverage and the profile. Returns the process exit code. */
static int finish_run(Frontend *f, int rc)
{
    Machine *m = f->m;
    if (f->o->headless && rc == 0) {
        if (frontend_audio_close(m) != 0) {
            rc = -1;
        } else if (f->o->screenshot) {
#ifdef INN_NATIVE
            int shot = m->native_video ? host_video_write_ppm(m, f->o->screenshot) : vga_write_ppm(m, f->o->screenshot);
#else
            int shot = vga_write_ppm(m, f->o->screenshot);
#endif
            if (shot != 0) {
                fprintf(stderr, "cannot write %s\n", f->o->screenshot);
                rc = -1;
            }
        }
    }
    if (f->script)
        input_script_free(f->script);
    f->script = NULL;
    int exit_code = rc != 0 ? 1 : m->exit_code;
    if (m->exit_requested)
        fprintf(stderr, "program exited with code %d\n", m->exit_code);
#ifdef HAVE_DOS
    dos_shutdown(m);
#endif
    if (m->exec_stub) {
        exec_stub_shutdown(m);
        host_rules_free(host_rules);
    }
    bios_shutdown(m);
    machine_destroy(m);
    f->m = NULL;
    return exit_code;
}

int main(int argc, char **argv)
{
    static Options o;
    if (parse_args(argc, argv, &o) != 0) {
        usage();
        return 2;
    }
    static Frontend f;
    f.o = &o;
    f.m = machine_create();
    f.m->trace = o.trace;
    if (o.ips)
        f.m->ips = o.ips;
    f.m->fpu_present = !o.no_fpu;
    f.m->idle.off = o.no_idle_skip;
    int rc = boot(f.m, &o);
    if (rc == 0)
        rc = o.headless ? start_headless(&f, &o) : start_windowed(&f, &o);
#ifdef INN_NATIVE
    if (rc == 0)
        rc = run_native(&f);
#else
    if (rc == 0)
        rc = o.headless ? run_headless(&f) : run_windowed(&f);
#endif
    return finish_run(&f, rc);
}
