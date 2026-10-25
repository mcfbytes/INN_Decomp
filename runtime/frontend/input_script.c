/* Script verbs: key, down, up, type, move, click, shot, signal, await, end (docs/runtime.md, "Input scripts"). */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "devices.h"
#include "host_input.h"
#include "host_video.h"
#include "input_script.h"

enum { MAX_EVENTS = 4096, HOLD_US = 60000, TYPE_GAP_US = 120000, LINE_MAX = 512, PATH_MAX_LEN = 256 };

typedef enum { EV_KEY_DOWN, EV_KEY_UP, EV_MOVE, EV_BUTTON_DOWN, EV_BUTTON_UP, EV_SHOT, EV_SIGNAL, EV_AWAIT, EV_END } EventKind;

typedef struct Event {
    uint64_t at_us;
    EventKind kind;
    int a, b;
    char path[PATH_MAX_LEN];
} Event;

struct InputScript {
    Event events[MAX_EVENTS];
    int count, next;
    uint64_t shift_us;       /* time spent in awaits: added to every event not yet applied */
    bool waiting;            /* held at the await in events[next - 1] */
    uint64_t wait_start_us, wait_deadline_us;
    bool ended;              /* an "end" event was applied */
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
    Event *e = &s->events[s->count++];
    *e = (Event){ .at_us = at_us, .kind = kind, .a = a, .b = b };
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
    } else if (!strcmp(verb, "shot") && *rest) {
        Event *e = add_event(s, at, EV_SHOT, 0, 0);
        if (e)
            snprintf(e->path, sizeof e->path, "%s", rest);
    } else if ((!strcmp(verb, "signal") || !strcmp(verb, "await")) && *rest) {
        char name[PATH_MAX_LEN];
        unsigned long long timeout_ms = 120000;
        if (verb[0] == 's' ? sscanf(rest, "%255s", name) != 1 : sscanf(rest, "%255s %llu", name, &timeout_ms) < 1)
            return -1;
        if (strchr(name, '/'))
            return -1;
        Event *e = add_event(s, at, verb[0] == 's' ? EV_SIGNAL : EV_AWAIT, 0, 0);
        if (e) {
            snprintf(e->path, sizeof e->path, "%s", name);
            e->b = (int)(timeout_ms > 86400000ULL ? 86400000ULL : timeout_ms);
        }
    } else if (!strcmp(verb, "end") && !*rest) {
        add_event(s, at, EV_END, 0, 0);
    } else {
        return -1;
    }
    return 0;
}

static int by_time(const void *a, const void *b)
{
    const Event *x = a, *y = b;
    return x->at_us < y->at_us ? -1 : x->at_us > y->at_us ? 1 : (x < y ? -1 : 1);
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

uint64_t input_script_next_us(const InputScript *s)
{
    if (s->waiting)
        return s->wait_start_us;
    return s->next < s->count ? s->events[s->next].at_us + s->shift_us : UINT64_MAX;
}

bool input_script_ended(const InputScript *s) { return s->ended; }

/* The sync file for NAME, or false when INN_SYNC_DIR is unset. */
static bool sync_path(char *out, size_t size, const char *name)
{
    const char *dir = getenv("INN_SYNC_DIR");
    if (!dir || !*dir)
        return false;
    return snprintf(out, size, "%s/%s", dir, name) < (int)size;
}

static bool sync_exists(const char *name)
{
    char path[PATH_MAX_LEN + 512];
    struct stat st;
    return sync_path(path, sizeof path, name) && stat(path, &st) == 0;
}

static void sync_signal(const char *name)
{
    char path[PATH_MAX_LEN + 512];
    if (!sync_path(path, sizeof path, name)) {
        log_msg("script: signal %s without INN_SYNC_DIR", name);
        return;
    }
    char *slash = strrchr(path, '/');
    *slash = 0;
    if (mkdir(path, 0777) != 0 && errno != EEXIST)
        log_msg("script: cannot create %s", path);
    *slash = '/';
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
    else
        log_msg("script: cannot create %s", path);
}

static void apply_event(Machine *m, const Event *e)
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
    default: break;
    }
}

void input_script_apply(InputScript *s, Machine *m)
{
    while (!s->ended) {
        if (s->waiting) {
            bool got = sync_exists(s->events[s->next - 1].path);
            if (!got && m->virtual_us < s->wait_deadline_us)
                return;
            if (!got)
                log_msg("script: await %s timed out", s->events[s->next - 1].path);
            s->shift_us += m->virtual_us - s->wait_start_us;
            s->waiting = false;
            continue;
        }
        if (s->next >= s->count || s->events[s->next].at_us + s->shift_us > m->virtual_us)
            return;
        const Event *e = &s->events[s->next++];
        if (e->kind == EV_SIGNAL) {
            sync_signal(e->path);
        } else if (e->kind == EV_END) {
            s->ended = true;
        } else if (e->kind == EV_AWAIT) {
            if (sync_exists(e->path))
                continue;
            s->waiting = true;
            s->wait_start_us = m->virtual_us;
            s->wait_deadline_us = m->virtual_us + (uint64_t)e->b * 1000;
        } else {
            apply_event(m, e);
        }
    }
}
