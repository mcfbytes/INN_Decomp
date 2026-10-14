/* SDL2 frontend: input into the machine, frames and sound out. Per-frame work is in frame_step so
 * the browser build can drive it from emscripten_set_main_loop; --headless needs no display. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "audio_sdl.h"
#include "lift.h"
#include "devices.h"
#include "frame_clock.h"
#include "input_script.h"
#include "machine.h"
#if __has_include("dos.h")
#include "dos.h"
#define HAVE_DOS 1
#endif

enum {
    WINDOW_W = 960, WINDOW_H = 720, /* 4:3, so 200-line modes get their tall pixels */
    HEADLESS_CHUNK_US = 10000,
    FRAME_PERIOD_US = 16667,
    NESTED_AHEAD_US = 2000,    /* emulated time a nested wait may run ahead of the wall clock */
    NESTED_PRESENT_US = 16667,
    DEFAULT_RUN_MS = 10000,
    ARGS_MAX = 128,
};

typedef struct Options {
    int trace;
    uint64_t ips;
    const char *com1;
    bool headless, perf;
    uint64_t run_ms;
    const char *screenshot;
    const char *script;
    const char *wav;
    unsigned conv_kb;
    const char *c_dir, *program;
    char args[ARGS_MAX];
} Options;

typedef struct Frontend {
    Machine *m;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t frame[VGA_MAX_W * VGA_MAX_H];
    int frame_w, frame_h;
    FrameClock clock;
    uint64_t step_wall_us, step_virtual_us; /* at the start of the current frame */
    uint64_t last_present_us;
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
    mouse_host_motion(f->m, wx * f->frame_w / WINDOW_W, wy * f->frame_h / WINDOW_H, f->frame_w, f->frame_h);
}

static void handle_event(Frontend *f, const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: f->quit = true; break;
    case SDL_KEYDOWN: case SDL_KEYUP: {
        uint16_t sc = to_set1(e->key.keysym.scancode);
        if (sc)
            kbd_host_key(f->m, sc, e->type == SDL_KEYDOWN);
        break;
    }
    case SDL_MOUSEMOTION: mouse_to_guest(f, e->motion.x, e->motion.y); break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        mouse_to_guest(f, e->button.x, e->button.y);
        mouse_host_button(f->m, to_guest_button(e->button.button), e->type == SDL_MOUSEBUTTONDOWN);
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

static void present(Frontend *f)
{
    f->last_present_us = frame_clock_now_us();
    vga_render(f->m, f->frame, &f->frame_w, &f->frame_h);
    draw_cursor_overlay(f);
    SDL_Rect src = { 0, 0, f->frame_w, f->frame_h };
    SDL_UpdateTexture(f->texture, &src, f->frame, f->frame_w * (int)sizeof f->frame[0]);
    SDL_RenderClear(f->renderer);
    SDL_RenderCopy(f->renderer, f->texture, &src, NULL);
    SDL_RenderPresent(f->renderer);
}

/* ---- main loop ---- */

static Frontend *g_frontend; /* emscripten_set_main_loop takes no context argument */

static void host_sleep_us(uint64_t us)
{
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
    uint64_t now = frame_clock_now_us();
    uint64_t emulated = m->virtual_us - f->step_virtual_us, wall = now - f->step_wall_us;
    if (emulated > wall + NESTED_AHEAD_US)
        host_sleep_us(emulated - wall);
    if (now - f->last_present_us < NESTED_PRESENT_US)
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
    frame_clock_run(&f->clock, f->m);
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
    f->window = SDL_CreateWindow("ImagiNation Network", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
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

static int run_windowed(Frontend *f, const Options *o)
{
    if (open_window(f) != 0) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        close_window(f);
        return -1;
    }
    f->frame_w = 320, f->frame_h = 200;
    frontend_audio_open(f->m, true, o->wav);
    frame_clock_start(&f->clock, f->m, o->perf);
    g_frontend = f;
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

static int run_headless(Frontend *f, const Options *o)
{
    uint64_t end = f->m->virtual_us + o->run_ms * 1000;
    InputScript *script = NULL;
    if (o->script && !(script = input_script_load(o->script))) {
        fprintf(stderr, "cannot load script %s\n", o->script);
        return -1;
    }
    if (o->wav && frontend_audio_open(f->m, false, o->wav) != 0)
        return -1;
    while (f->m->virtual_us < end && !f->m->exit_requested) {
        if (script)
            input_script_apply(script, f->m);
        machine_run_for(f->m, HEADLESS_CHUNK_US);
        frontend_audio_pump(f->m);
    }
    if (script)
        input_script_free(script);
    if (frontend_audio_close(f->m) != 0)
        return -1;
    if (o->screenshot && vga_write_ppm(f->m, o->screenshot) != 0) {
        fprintf(stderr, "cannot write %s\n", o->screenshot);
        return -1;
    }
    return 0;
}

/* ---- command line ---- */

static void usage(void)
{
    fprintf(stderr, "usage: inn [--trace N] [--ips N] [--com1 host:port|ws://host:port] [--perf] [--wav out.wav] [--conv-kb N] "
                    "[--headless --run-ms N --screenshot out.ppm --script input.txt] <c-drive-dir> <program> [args]\n");
}

static int parse_args(int argc, char **argv, Options *o)
{
    *o = (Options){ .run_ms = DEFAULT_RUN_MS };
    int i = 1;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--headless")) { o->headless = true; continue; }
        if (!strcmp(a, "--perf")) { o->perf = true; continue; }
        if (!next)
            return -1;
        if (!strcmp(a, "--trace")) o->trace = atoi(next);
        else if (!strcmp(a, "--ips")) o->ips = strtoull(next, NULL, 10);
        else if (!strcmp(a, "--com1")) o->com1 = next;
        else if (!strcmp(a, "--run-ms")) o->run_ms = strtoull(next, NULL, 10);
        else if (!strcmp(a, "--screenshot")) o->screenshot = next;
        else if (!strcmp(a, "--script")) o->script = next;
        else if (!strcmp(a, "--wav")) o->wav = next;
        else if (!strcmp(a, "--conv-kb")) o->conv_kb = (unsigned)strtoul(next, NULL, 10);
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

static int boot(Machine *m, const Options *o)
{
    bios_init(m);
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

int main(int argc, char **argv)
{
    Options o;
    if (parse_args(argc, argv, &o) != 0) {
        usage();
        return 2;
    }
    static Frontend f;
    f.m = machine_create();
    f.m->trace = o.trace;
    if (o.ips)
        f.m->ips = o.ips;
    int rc = boot(f.m, &o);
    if (rc == 0)
        rc = o.headless ? run_headless(&f, &o) : run_windowed(&f, &o);
    int exit_code = rc != 0 ? 1 : f.m->exit_code;
    if (f.m->exit_requested)
        fprintf(stderr, "program exited with code %d\n", f.m->exit_code);
#ifdef HAVE_DOS
    dos_shutdown(f.m);
#endif
    bios_shutdown(f.m);
    machine_destroy(f.m);
    return exit_code;
}
