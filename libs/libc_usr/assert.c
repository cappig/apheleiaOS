#include <assert.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void abort(void) {
    (void)raise(SIGABRT);
    _exit(134);
}

void __assert_fail(
    const char *expr,
    const char *file,
    int line,
    const char *func
) {
    fprintf(
        stderr,
        "assertion failed: %s (%s:%d: %s)\n",
        expr ? expr : "(null)",
        file ? file : "(unknown)",
        line,
        func ? func : "(unknown)"
    );
    abort();
}
