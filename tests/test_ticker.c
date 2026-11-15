/*
 * M3: the ticker from examples/ticker.c prints exactly the expected output
 * under virtual time.
 *
 * The test compiles the example's own source, with its main() renamed, so
 * that the task under test is the one the example runs. It captures what the
 * task prints by pointing stdout at a temporary file for the run.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define main ticker_example_main
#include "examples/ticker.c"
#undef main

#include "tests/test.h"

static char captured[256];

/* Runs the loop with stdout redirected into `captured`. */
static void run_and_capture(runtime_t *rt)
{
    FILE *tmp = tmpfile();
    int saved;
    size_t n;

    CHECK(tmp != NULL);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    CHECK(saved >= 0);
    CHECK(dup2(fileno(tmp), STDOUT_FILENO) >= 0);

    CHECK(runtime_run(rt) == 0);

    fflush(stdout);
    CHECK(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);

    rewind(tmp);
    n = fread(captured, 1, sizeof captured - 1, tmp);
    captured[n] = '\0';
    fclose(tmp);
}

int main(void)
{
    runtime_t *rt = runtime_new();
    char expected[256];
    task_t *t;

    CHECK(rt != NULL);
    runtime_set_virtual_time(rt);
    CHECK(runtime_now_ms(rt) == 0);

    t = task_spawn(rt, ticker, sizeof(struct ticker));
    CHECK(t != NULL);

    run_and_capture(rt);

    snprintf(expected, sizeof expected,
             "tick 0\ntick 1\ntick 2\nticker finished: %s\n", strerror(ETIMEDOUT));
    CHECK(strcmp(captured, expected) == 0);

    /* The deadline, not the third sleep, ended the task. */
    CHECK(task_error(t) == ETIMEDOUT);
    CHECK(runtime_now_ms(rt) == 250);

    task_detach(t);
    CHECK_NOTHING_LEFT(rt);
    runtime_free(rt);
    return 0;
}
