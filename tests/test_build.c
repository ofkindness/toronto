/*
 * M0: the build keeps the promises the rest of the project relies on.
 *
 *   - the code is compiled as ISO C11;
 *   - asserts stay enabled in every build;
 *   - POSIX.1-2008 exposes the monotonic clock that the timer heap will read.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <time.h>

#if __STDC_VERSION__ != 201112L
#error "toronto is built as ISO C11 (-std=c11)"
#endif

#ifdef NDEBUG
#error "asserts stay enabled in every build: do not define NDEBUG"
#endif

int main(void)
{
    struct timespec a, b;

    if (clock_gettime(CLOCK_MONOTONIC, &a) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &b) != 0) {
        perror("clock_gettime(CLOCK_MONOTONIC)");
        return 1;
    }
    if (b.tv_sec < a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec < a.tv_nsec)) {
        fprintf(stderr, "CLOCK_MONOTONIC went backwards\n");
        return 1;
    }
    return 0;
}
