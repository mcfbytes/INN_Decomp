/* Input scripts (docs/runtime.md): parsing, and "signal"/"wait" between two headless clients: a wait holds
 * the emulated clock until the other run's file exists, and fails the run after its wall-time limit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "devices.h"
#include "input_script.h"

static int failures;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            failures++;                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fputc('\n', stderr);                                 \
        }                                                        \
    } while (0)

static char dir[256];

static InputScript *load_text(const char *text)
{
    char path[300];
    snprintf(path, sizeof path, "%s/script.txt", dir);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
    return input_script_load(path);
}

static bool exists(const char *name)
{
    char path[300];
    snprintf(path, sizeof path, "%s/sync/%s", dir, name);
    return access(path, F_OK) == 0;
}

static void touch(const char *name)
{
    char path[300];
    snprintf(path, sizeof path, "%s/sync/%s", dir, name);
    fclose(fopen(path, "w"));
}

static int holds;
static const char *signal_on_hold; /* the other client signals while this one waits */

static void count_hold(void *ctx)
{
    (void)ctx;
    if (++holds == 3 && signal_on_hold)
        touch(signal_on_hold);
}

static void test_parse(void)
{
    InputScript *s = load_text("# comment\n1000 key 1C\n2000 signal ready\n3000 wait go\n4000 wait go2 500\n\n");
    CHECK(s, "valid script refused");
    if (s) {
        CHECK(input_script_next_us(s) == 1001000, "first event at %llu", (unsigned long long)input_script_next_us(s));
        input_script_free(s);
    }
    static const char *bad[] = {
        "1000 signal\n", "1000 wait\n", "1000 signal a b\n", "1000 wait a 10 x\n", "1000 wait a 0\n",
        "1000 signal ../x\n", "1000 signal .hidden\n", "1000 wait a/b\n", "1000 bogus\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        InputScript *b = load_text(bad[i]);
        CHECK(!b, "accepted bad line %s", bad[i]);
        input_script_free(b);
    }
}

static void test_signal_and_wait(Machine *m)
{
    char sync[300];
    snprintf(sync, sizeof sync, "%s/sync", dir);
    mkdir(sync, 0755);
    static const char *names[] = { "mine", "theirs", "after", "never", "not_reached", "x" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[400];
        snprintf(path, sizeof path, "%s/%s", sync, names[i]);
        unlink(path); /* left by an earlier run */
    }

    /* Events at one time keep file order: the signal lands before the wait that follows it. */
    InputScript *s = load_text("1000 signal mine\n1000 wait mine\n2000 wait theirs 5000\n3000 signal after\n");
    input_script_set_sync(s, sync, count_hold, NULL);
    m->virtual_us = 1000000;
    holds = 0;
    CHECK(input_script_apply(s, m) == 0 && exists("mine") && holds == 0, "own signal satisfies the wait");
    m->virtual_us = 3000000;
    signal_on_hold = "theirs";
    CHECK(input_script_apply(s, m) == 0, "wait for the other side");
    CHECK(holds == 3, "held %d times", holds);
    CHECK(m->virtual_us == 3000000, "the clock moved during the wait");
    CHECK(exists("after"), "events after the wait run once it is over");
    CHECK(input_script_next_us(s) == UINT64_MAX, "events left");
    input_script_free(s);

    /* Nobody signals: the run fails after the limit and applies nothing more. */
    s = load_text("1000 wait never 100\n1000 signal not_reached\n");
    input_script_set_sync(s, sync, count_hold, NULL);
    signal_on_hold = NULL;
    m->virtual_us = 1000000;
    CHECK(input_script_apply(s, m) == -1 && input_script_failed(s), "timeout fails the run");
    CHECK(!exists("not_reached"), "a failed script went on");
    CHECK(input_script_apply(s, m) == -1, "failure is sticky");
    input_script_free(s);

    /* Without a sync directory the commands fail rather than pass silently. */
    s = load_text("1000 signal x\n");
    m->virtual_us = 1000000;
    CHECK(input_script_apply(s, m) == -1, "signal without --sync-dir");
    input_script_free(s);
}

int main(void)
{
    snprintf(dir, sizeof dir, "test_input_script.d");
    mkdir(dir, 0755);
    Machine *m = machine_create();
    test_parse();
    test_signal_and_wait(m);
    machine_destroy(m);
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        puts("test_input_script: ok");
    return failures ? 1 : 0;
}
