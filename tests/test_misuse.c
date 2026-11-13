/*
 * M1: programming errors abort. Each scenario runs in a child process, and
 * the test checks that the child called abort().
 *
 * The child turns SIGABRT into a distinct exit code. That tells an abort
 * apart from every other way to die, and it keeps the operating system from
 * writing a crash report for an abort that the test asked for.
 */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "runtime/async.h"
#include "runtime/runtime.h"
#include "runtime/task.h"
#include "tests/test.h"

enum { EXIT_ABORTED = 86 };

static void on_abort(int sig)
{
    (void)sig;
    _exit(EXIT_ABORTED);
}

static void run_one(task_fn fn)
{
    runtime_t *rt = runtime_new();
    task_t *t;

    if (rt == NULL)
        return;
    t = task_spawn(rt, fn, 0);
    if (t == NULL)
        return;
    task_detach(t);
    runtime_run(rt);
    runtime_free(rt);
}

/* Runs fn as a task in a child process and reports whether the child aborted. */
static int aborts(task_fn fn)
{
    int status = 0;
    pid_t pid = fork();

    CHECK(pid >= 0);
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);   /* the abort message is expected */
        signal(SIGABRT, on_abort);
        run_one(fn);
        _exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) && WEXITSTATUS(status) == EXIT_ABORTED;
}

/* A well-behaved task: the harness must not report an abort for it. */
static poll_t behaves(task_t *t)
{
    ASYNC_BEGIN(t);
    YIELD(t);
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

/* Rule 6: the cleanup section must not suspend. */
static poll_t yields_in_cleanup(task_t *t)
{
    ASYNC_BEGIN(t);
    ASYNC_CLEANUP(t);
    YIELD(t);
    ASYNC_END(t);
}

/* An error code of 0 would mean success. */
static poll_t fails_with_zero(task_t *t)
{
    ASYNC_BEGIN(t);
    ASYNC_FAIL(t, 0);
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

/*
 * Rule 2: no YIELD inside a switch statement. The YIELD's case label belongs
 * to the inner switch, so the outer one cannot find the resume point.
 */
static poll_t yields_in_switch(task_t *t)
{
    int one = 1;

    ASYNC_BEGIN(t);
    switch (one) {
    case 1:
        YIELD(t);
        break;
    }
    ASYNC_CLEANUP(t);
    ASYNC_END(t);
}

int main(void)
{
    CHECK(!aborts(behaves));
    CHECK(aborts(yields_in_cleanup));
    CHECK(aborts(fails_with_zero));
    CHECK(aborts(yields_in_switch));
    return 0;
}
