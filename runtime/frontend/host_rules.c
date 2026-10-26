/* Scripted host for the TSNEXEC stand-in (--tsn-host FILE): shared block, previous program and
 * pattern replies; the rule format is in docs/runtime.md, "TSN host stand-in". */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "exec_stub.h"
#include "host_rules.h"

enum { RULES_MAX = 128, ITEMS_MAX = 96, ANY = -1 };

typedef enum { T_BYTE, T_REQ, T_REQ_REST, T_TIME, T_REQ_W, T_REQ_D } ItemKind;
/* T_REQ_W / T_REQ_D: value is the request offset, delta the signed amount added (mod 2^16 / 2^32). */
typedef struct Item { ItemKind kind; int value; int32_t delta; } Item;

typedef enum { M_ALWAYS, M_ONCE, M_NTH, M_AT } RuleMode;

typedef struct Rule {
    int pattern[ITEMS_MAX];
    int pattern_len;
    Item tmpl[ITEMS_MAX];
    int tmpl_len;
    RuleMode mode;
    int nth;      /* M_NTH: the matching message that answers (1-based) */
    int seen;     /* matches so far */
    long at_ms;   /* M_AT: emulated time at which the reply is queued */
    bool done;    /* M_ONCE / M_NTH / M_AT: already answered */
} Rule;

struct HostRules {
    Rule rules[RULES_MAX];
    int count;
};

/* Splits on whitespace, keeping "quoted text" as one token (quotes included). */
static int tokenize(char *line, char **tok, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (isspace((unsigned char)*p))
            p++;
        if (!*p || *p == '#')
            break;
        tok[n++] = p;
        if (*p == '"') {
            char *end = strchr(p + 1, '"');
            p = end ? end + 1 : p + strlen(p);
        } else {
            while (*p && !isspace((unsigned char)*p))
                p++;
        }
        if (*p)
            *p++ = 0;
    }
    return n;
}

/* Appends one template token; false if it is not valid. */
static bool parse_item(const char *t, Item *items, int *n)
{
    if (t[0] == '"') {
        for (const char *c = t + 1; *c && *c != '"' && *n < ITEMS_MAX; c++)
            items[(*n)++] = (Item){T_BYTE, (unsigned char)*c, 0};
        return true;
    }
    if (*n >= ITEMS_MAX)
        return false;
    if (t[0] == '@') {
        char *end;
        long v = strtol(t + 1, &end, 10);
        if (end == t + 1)
            return false;
        if (*end == 'w' || *end == 'd') {
            ItemKind kind = *end == 'w' ? T_REQ_W : T_REQ_D;
            int32_t delta = 0;
            if (end[1]) {
                char sign = end[1];
                char *kend;
                long k = strtol(end + 2, &kend, 16);
                if ((sign != '+' && sign != '-') || kend == end + 2 || *kend)
                    return false;
                delta = (int32_t)(sign == '-' ? -k : k);
            }
            items[(*n)++] = (Item){kind, (int)v, delta};
            return true;
        }
        items[(*n)++] = (Item){*end == '-' ? T_REQ_REST : T_REQ, (int)v, 0};
        return true;
    }
    if (!strcmp(t, "T")) {
        items[(*n)++] = (Item){T_TIME, 0, 0};
        return true;
    }
    char *end;
    long v = strtol(t, &end, 16);
    items[(*n)++] = (Item){T_BYTE, (int)v, 0};
    return *end == 0 && v >= 0 && v <= 255;
}

HostRules *host_rules_load(Machine *m, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    HostRules *s = calloc(1, sizeof *s);
    char line[1024];
    int lineno = 0;
    bool ok = true;
    while (ok && fgets(line, sizeof line, f)) {
        lineno++;
        char *tok[ITEMS_MAX * 2];
        int n = tokenize(line, tok, ITEMS_MAX * 2);
        if (n == 0)
            continue;
        if (!strcmp(tok[0], "shared") && n >= 2) {
            Item items[ITEMS_MAX];
            int count = 0;
            for (int i = 2; i < n && ok; i++)
                ok = parse_item(tok[i], items, &count);
            uint8_t bytes[ITEMS_MAX];
            for (int i = 0; i < count; i++)
                bytes[i] = (uint8_t)items[i].value;
            exec_stub_set_shared(m, (uint16_t)strtoul(tok[1], NULL, 16), bytes, (uint16_t)count);
        } else if (!strcmp(tok[0], "previous") && n == 2) {
            exec_stub_set_previous(m, tok[1]);
        } else if (!strcmp(tok[0], "reply") && s->count < RULES_MAX) {
            Rule *r = &s->rules[s->count++];
            int i = 1;
            if (i < n && !strcmp(tok[i], "once")) {
                r->mode = M_ONCE;
                i++;
            } else if (i + 1 < n && !strcmp(tok[i], "nth")) {
                r->mode = M_NTH;
                r->nth = atoi(tok[i + 1]);
                ok = r->nth > 0;
                i += 2;
            }
            for (; i < n && strcmp(tok[i], "=>"); i++)
                r->pattern[r->pattern_len++] = !strcmp(tok[i], "..") ? ANY : (int)strtol(tok[i], NULL, 16);
            for (i++; i < n && ok; i++)
                ok = parse_item(tok[i], r->tmpl, &r->tmpl_len);
        } else if (!strcmp(tok[0], "at") && n >= 3 && !strcmp(tok[2], "=>") && s->count < RULES_MAX) {
            Rule *r = &s->rules[s->count++];
            r->mode = M_AT;
            r->at_ms = atol(tok[1]);
            for (int i = 3; i < n && ok; i++)
                ok = parse_item(tok[i], r->tmpl, &r->tmpl_len);
        } else {
            ok = false;
        }
        if (!ok)
            fprintf(stderr, "%s:%d: cannot parse\n", path, lineno);
    }
    fclose(f);
    if (!ok) {
        free(s);
        return NULL;
    }
    return s;
}

static void answer(Machine *m, const Rule *r, const uint8_t *req, uint16_t len)
{
    uint8_t out[1024];
    int n = 0;
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    for (int i = 0; i < r->tmpl_len && n < (int)sizeof out - 8; i++) {
        const Item *it = &r->tmpl[i];
        switch (it->kind) {
        case T_BYTE: out[n++] = (uint8_t)it->value; break;
        case T_REQ: out[n++] = it->value < len ? req[it->value] : 0; break;
        case T_REQ_W: {
            uint16_t v = 0;
            for (int k = 0; k < 2; k++)
                v |= (uint16_t)((it->value + k < len ? req[it->value + k] : 0) << (8 * k));
            v = (uint16_t)(v + it->delta);
            out[n++] = (uint8_t)v, out[n++] = (uint8_t)(v >> 8);
            break;
        }
        case T_REQ_D: {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++)
                v |= (uint32_t)(it->value + k < len ? req[it->value + k] : 0) << (8 * k);
            v += (uint32_t)it->delta;
            for (int k = 0; k < 4; k++)
                out[n++] = (uint8_t)(v >> (8 * k));
            break;
        }
        case T_REQ_REST:
            for (int k = it->value; k < len && n < (int)sizeof out; k++)
                out[n++] = req[k];
            break;
        case T_TIME:
            out[n++] = (uint8_t)tm.tm_year, out[n++] = (uint8_t)tm.tm_mon, out[n++] = (uint8_t)tm.tm_mday;
            out[n++] = (uint8_t)tm.tm_hour, out[n++] = (uint8_t)tm.tm_min, out[n++] = (uint8_t)tm.tm_sec;
            break;
        }
    }
    exec_stub_deliver(m, out, (uint16_t)n);
}

void host_rules_poll(Machine *m, void *user)
{
    HostRules *s = user;
    for (int i = 0; i < s->count; i++) {
        Rule *r = &s->rules[i];
        if (r->mode == M_AT && !r->done && (long)(m->virtual_us / 1000) >= r->at_ms) {
            r->done = true;
            answer(m, r, NULL, 0);
        }
    }
}

void host_rules_on_send(Machine *m, const uint8_t *msg, uint16_t len, void *user)
{
    HostRules *s = user;
    char hex[3 * 48 + 1] = "";
    for (int i = 0; i < len && i < 48; i++)
        snprintf(hex + 3 * i, 4, " %02x", msg[i]);
    log_msg("tsn: client sent%s%s", hex, len > 48 ? " ..." : "");
    host_rules_poll(m, s);
    for (int i = 0; i < s->count; i++) {
        Rule *r = &s->rules[i];
        if (r->mode == M_AT)
            continue;
        bool match = r->pattern_len <= len;
        for (int k = 0; match && k < r->pattern_len; k++)
            match = r->pattern[k] == ANY || r->pattern[k] == msg[k];
        if (!match)
            continue;
        r->seen++;
        if ((r->mode == M_ONCE && r->seen > 1) || (r->mode == M_NTH && r->seen != r->nth))
            continue;
        answer(m, r, msg, len);
    }
}

void host_rules_free(HostRules *s) { free(s); }
