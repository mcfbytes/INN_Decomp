/* Host rule language (runtime/frontend/host_rules.c): once, nth, word arithmetic, timed replies, and
 * that the older forms keep their meaning. exec_stub is replaced by recording stand-ins. */
#include <stdio.h>
#include <string.h>

#include "host_rules.h"

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            failures++;                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
        }                                                      \
    } while (0)

static uint8_t got[16][64];
static int got_len[16], got_count;

void exec_stub_deliver(Machine *m, const uint8_t *msg, uint16_t len)
{
    (void)m;
    if (got_count < 16) {
        memcpy(got[got_count], msg, len);
        got_len[got_count++] = len;
    }
}
void exec_stub_set_shared(Machine *m, uint16_t o, const uint8_t *d, uint16_t n) { (void)m, (void)o, (void)d, (void)n; }
void exec_stub_set_previous(Machine *m, const char *name) { (void)m, (void)name; }

static const char *RULES =
    "reply 01 => aa @1\n"
    "reply once 02 => bb\n"
    "reply nth 2 03 => cc\n"
    "reply 04 => @1w+10 @1w-10 @1d+1 @2-\n"
    "reply 05 => @9w+1 @9d-1\n"
    "at 500 => dd ee\n";

static bool expect(int idx, const uint8_t *bytes, int len)
{
    return idx < got_count && got_len[idx] == len && !memcmp(got[idx], bytes, (size_t)len);
}

int main(void)
{
    const char *path = "test_host_rules.txt";
    FILE *f = fopen(path, "w");
    if (!f)
        return 1;
    fputs(RULES, f);
    fclose(f);
    Machine *m = machine_create();
    HostRules *h = host_rules_load(m, path);
    CHECK(h != NULL, "rules parse");
    if (!h)
        return 1;

    const uint8_t m01[] = {1, 7}, m02[] = {2}, m03[] = {3};
    host_rules_on_send(m, m01, sizeof m01, h);
    CHECK(expect(0, (const uint8_t[]){0xAA, 7}, 2), "plain rule");
    host_rules_on_send(m, m01, sizeof m01, h);
    CHECK(got_count == 2, "plain rule answers every time");

    got_count = 0;
    host_rules_on_send(m, m02, sizeof m02, h);
    host_rules_on_send(m, m02, sizeof m02, h);
    CHECK(got_count == 1 && expect(0, (const uint8_t[]){0xBB}, 1), "once answers the first match only (%d)", got_count);

    got_count = 0;
    host_rules_on_send(m, m03, sizeof m03, h);
    CHECK(got_count == 0, "nth 2 is silent on the first match");
    host_rules_on_send(m, m03, sizeof m03, h);
    host_rules_on_send(m, m03, sizeof m03, h);
    CHECK(got_count == 1 && expect(0, (const uint8_t[]){0xCC}, 1), "nth 2 answers the second match only (%d)", got_count);

    got_count = 0;
    const uint8_t m04[] = {4, 0xF8, 0xFF, 0xFF, 0xFF, 9};
    host_rules_on_send(m, m04, sizeof m04, h);
    /* word 0xFFF8 + 0x10 wraps to 0x0008; - 0x10 gives 0xFFE8; dword 0xFFFFFFF8... +1; rest from byte 2 */
    CHECK(expect(0, (const uint8_t[]){0x08, 0x00, 0xE8, 0xFF, 0xF9, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 9}, 12),
          "word and dword arithmetic, len %d first %02x%02x", got_len[0], got[0][0], got[0][1]);

    got_count = 0;
    const uint8_t m05[] = {5, 0, 0, 0, 0, 0, 0, 0, 0, 0x34, 0x12, 0, 0};
    host_rules_on_send(m, m05, sizeof m05, h);
    CHECK(expect(0, (const uint8_t[]){0x35, 0x12, 0x33, 0x12, 0, 0}, 6), "offset 9 word +1 and dword -1");

    got_count = 0;
    m->virtual_us = 499000;
    host_rules_poll(m, h);
    CHECK(got_count == 0, "at 500 not before its time");
    m->virtual_us = 500000;
    host_rules_poll(m, h);
    host_rules_poll(m, h);
    CHECK(got_count == 1 && expect(0, (const uint8_t[]){0xDD, 0xEE}, 2), "at 500 queued once (%d)", got_count);

    host_rules_free(h);
    remove(path);

    /* a malformed word suffix is refused */
    f = fopen(path, "w");
    fputs("reply 01 => @1w*3\n", f);
    fclose(f);
    CHECK(host_rules_load(m, path) == NULL, "bad suffix refused");
    remove(path);

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        printf("test_host_rules: all passed\n");
    return failures ? 1 : 0;
}
