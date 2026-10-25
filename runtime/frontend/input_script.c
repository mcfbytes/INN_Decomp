/* Script lines: "<ms> key|down|up <set1-hex>", "type <text>", "move|click <x> <y>", "shot <file.ppm>",
 * "signal <name>", "wait <name> [timeout_ms]"; '#' starts a comment (docs/runtime.md). */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "devices.h"
#include "host_input.h"
#include "host_video.h"
#include "input_script.h"

enum { MAX_EVENTS = 4096, HOLD_US = 60000, TYPE_GAP_US = 120000, LINE_MAX = 512, PATH_MAX_LEN = 256,
       SYNC_NAME_MAX = 64, WAIT_DEFAULT_MS = 120000, WAIT_POLL_US = 10000 };

typedef enum { EV_KEY_DOWN, EV_KEY_UP, EV_MOVE, EV_BUTTON_DOWN, EV_BUTTON_UP, EV_SHOT, EV_SIGNAL, EV_WAIT } EventKind;

typedef struct Event {
    uint64_t at_us;
    EventKind kind;
    int a, b;
    int seq; /* file order, for events at the same time */
    char path[PATH_MAX_LEN];
} Event;

struct InputScript {
    Event events[MAX_EVENTS];
    int count, next;
    bool failed;
    char sync_dir[PATH_MAX_LEN];
    void (*hold)(void *ctx);
    void *hold_ctx;
};

/* Set-1 make codes for the US layout; index is the ASCII character, 0x100 marks Shift. */
static uint16_t ascii_to_set1(char ch)
{
    static const char row1[] = "1234567890-=", row2[] = "qwertyuiop[]", row3[] = "asdfghjkl;'`";
    static const char row4[] = "\\zxcvbnm,./", shifted[] = "!@#$%^&*()_+", lower_of_shifted[] = "1234567890-=";
    const char *hit;
    uint16_t shift = 0;
    if (isupper((unsigned char)ch)) {
        ch = (char)tolower((unsigned char)ch);
        shift = 0x100;
    } else if ((hit = strchr(shifted, ch)) && ch) {
        ch = lower_of_shifted[hit - shifted];
        shift = 0x100;
    }
    if (ch == ' ') return 0x39;
    if (ch == '\n') return 0x1C;
    if ((hit = strchr(row1, ch)) && ch) return shift | (uint16_t)(0x02 + (hit - row1));
    if ((hit = strchr(row2, ch)) && ch) return shift | (uint16_t)(0x10 + (hit - row2));
    if ((hit = strchr(row3, ch)) && ch) return shift | (uint16_t)(0x1E + (hit - row3));
    if ((hit = strchr(row4, ch)) && ch) return shift | (uint16_t)(0x2B + (hit - row4));
    return 0;
}

static Event *add_event(InputScript *s, uint64_t at_us, EventKind kind, int a, int b)
{
    if (s->count >= MAX_EVENTS)
        return NULL;
    Event *e = &s->events[s->count];
    *e = (Event){ .at_us = at_us, .kind = kind, .a = a, .b = b, .seq = s->count++ };
    return e;
}

static void add_keystroke(InputScript *s, uint64_t at_us, uint16_t code)
{
    if (code & 0x100) {
        add_event(s, at_us, EV_KEY_DOWN, 0x2A, 0);
        add_event(s, at_us + HOLD_US, EV_KEY_UP, 0x2A, 0);
    }
    add_event(s, at_us + 1000, EV_KEY_DOWN, code & 0xFFFF & ~0x100, 0);
    add_event(s, at_us + HOLD_US - 1000, EV_KEY_UP, code & 0xFFFF & ~0x100, 0);
}

static int parse_line(InputScript *s, char *line)
{
    char *hash = strchr(line, '#');
    if (hash)
        *hash = 0;
    unsigned long long ms;
    char verb[16];
    int used = 0;
    if (sscanf(line, " %llu %15s %n", &ms, verb, &used) < 2)
        return strspn(line, " \t\r\n") == strlen(line) ? 0 : -1;
    uint64_t at = ms * 1000;
    char *rest = line + used;
    rest[strcspn(rest, "\r\n")] = 0;
    int x, y;
    unsigned code;
    if (!strcmp(verb, "key") && sscanf(rest, "%x", &code) == 1) {
        add_keystroke(s, at, (uint16_t)code);
    } else if ((!strcmp(verb, "down") || !strcmp(verb, "up")) && sscanf(rest, "%x", &code) == 1) {
        add_event(s, at, verb[0] == 'd' ? EV_KEY_DOWN : EV_KEY_UP, (int)(code & 0xFFFF), 0);
    } else if (!strcmp(verb, "type")) {
        for (const char *p = rest; *p; p++, at += TYPE_GAP_US) {
            uint16_t c = p[0] == '\\' && p[1] == 'n' ? (p++, 0x1C) : ascii_to_set1(*p);
            if (c)
                add_keystroke(s, at, c);
        }
    } else if ((!strcmp(verb, "move") || !strcmp(verb, "click")) && sscanf(rest, "%d %d", &x, &y) == 2) {
        add_event(s, at, EV_MOVE, x, y);
        if (verb[0] == 'c') {
            add_event(s, at + HOLD_US, EV_BUTTON_DOWN, 0, 0);
            add_event(s, at + 2 * HOLD_US, EV_BUTTON_UP, 0, 0);
        }
    } else if ((!strcmp(verb, "mdown") || !strcmp(verb, "mup")) && !*rest) {
        add_event(s, at, verb[1] == 'd' ? EV_BUTTON_DOWN : EV_BUTTON_UP, 0, 0);
    } else if (!strcmp(verb, "shot") && *rest) {
        Event *e = add_event(s, at, EV_SHOT, 0, 0);
        if (e)
            snprintf(e->path, sizeof e->path, "%s", rest);
    } else if (!strcmp(verb, "signal") || !strcmp(verb, "wait")) {
        char name[SYNC_NAME_MAX + 1];
        unsigned long timeout_ms = WAIT_DEFAULT_MS;
        int end = 0, more = 0;
        bool is_wait = verb[0] == 'w';
        if (sscanf(rest, "%64s%n", name, &end) != 1)
            return -1;
        if (is_wait && sscanf(rest + end, "%lu%n", &timeout_ms, &more) == 1)
            end += more;
        /* A name is one plain file in the sync directory. */
        if (strspn(rest + end, " \t") != strlen(rest + end) || strlen(name) >= SYNC_NAME_MAX || strchr(name, '/') ||
            name[0] == '.' || timeout_ms == 0)
            return -1;
        Event *e = add_event(s, at, is_wait ? EV_WAIT : EV_SIGNAL, (int)(timeout_ms > 0x7FFFFFFF ? 0x7FFFFFFF : timeout_ms), 0);
        if (e)
            snprintf(e->path, sizeof e->path, "%s", name);
    } else {
        return -1;
    }
    return 0;
}

static int by_time(const void *a, const void *b)
{
    const Event *x = a, *y = b;
    return x->at_us < y->at_us ? -1 : x->at_us > y->at_us ? 1 : x->seq - y->seq;
}

InputScript *input_script_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    InputScript *s = calloc(1, sizeof *s);
    char line[LINE_MAX];
    for (int n = 1; s && fgets(line, sizeof line, f); n++) {
        if (parse_line(s, line) != 0) {
            log_msg("%s:%d: bad script line", path, n);
            free(s);
            s = NULL;
        }
    }
    fclose(f);
    if (s)
        qsort(s->events, (size_t)s->count, sizeof s->events[0], by_time);
    return s;
}

void input_script_free(InputScript *s) { free(s); }

void input_script_set_sync(InputScript *s, const char *dir, void (*hold)(void *ctx), void *ctx)
{
    snprintf(s->sync_dir, sizeof s->sync_dir, "%s", dir ? dir : "");
    s->hold = hold;
    s->hold_ctx = ctx;
}

bool input_script_failed(const InputScript *s) { return s->failed; }

static uint64_t wall_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static bool sync_path(InputScript *s, const Event *e, char *path, size_t cap)
{
    if (!s->sync_dir[0]) {
        log_msg("script: %s %s needs --sync-dir", e->kind == EV_WAIT ? "wait" : "signal", e->path);
        return false;
    }
    snprintf(path, cap, "%s/%s", s->sync_dir, e->path);
    return true;
}

static bool sync_signal(InputScript *s, const Event *e)
{
    char path[2 * PATH_MAX_LEN];
    if (!sync_path(s, e, path, sizeof path))
        return false;
    FILE *f = fopen(path, "w");
    if (!f) {
        log_msg("script: signal %s: cannot create %s: %s", e->path, path, strerror(errno));
        return false;
    }
    fclose(f);
    log_msg("script: signal %s", e->path);
    return true;
}

/* The emulated clock stands still meanwhile; the hold callback keeps the link drained. */
static bool sync_wait(InputScript *s, const Event *e)
{
    char path[2 * PATH_MAX_LEN];
    if (!sync_path(s, e, path, sizeof path))
        return false;
    uint64_t start = wall_us(), limit = (uint64_t)e->a * 1000u;
    while (access(path, F_OK) != 0) {
        if (wall_us() - start >= limit) {
            log_msg("script: wait %s timed out after %d ms", e->path, e->a);
            return false;
        }
        if (s->hold)
            s->hold(s->hold_ctx);
        struct timespec pause = { 0, WAIT_POLL_US * 1000L };
        nanosleep(&pause, NULL);
    }
    log_msg("script: wait %s done after %llu ms", e->path, (unsigned long long)((wall_us() - start) / 1000u));
    return true;
}

uint64_t input_script_next_us(const InputScript *s)
{
    return s->next < s->count ? s->events[s->next].at_us : UINT64_MAX;
}

static bool apply_event(InputScript *s, Machine *m, const Event *e)
{
    static uint32_t frame[VGA_MAX_W * VGA_MAX_H];
    int w, h;
    switch (e->kind) {
    case EV_KEY_DOWN: host_input_key(m, (uint16_t)e->a, true); break;
    case EV_KEY_UP: host_input_key(m, (uint16_t)e->a, false); break;
    case EV_MOVE:
        vga_render(m, frame, &w, &h);
        host_input_mouse_motion(m, e->a, e->b, w, h);
        break;
    case EV_BUTTON_DOWN: host_input_mouse_button(m, e->a, true); break;
    case EV_BUTTON_UP: host_input_mouse_button(m, e->a, false); break;
    case EV_SHOT:
        /* M9 stand-in until package 6: native mode takes its shots from the host video. */
        if ((m->native_video ? host_video_write_ppm(m, e->path) : vga_write_ppm(m, e->path)) != 0)
            log_msg("script: cannot write %s", e->path);
        break;
    case EV_SIGNAL: return sync_signal(s, e);
    case EV_WAIT: return sync_wait(s, e);
    }
    return true;
}

int input_script_apply(InputScript *s, Machine *m)
{
    while (!s->failed && s->next < s->count && s->events[s->next].at_us <= m->virtual_us)
        if (!apply_event(s, m, &s->events[s->next++]))
            s->failed = true;
    return s->failed ? -1 : 0;
}
