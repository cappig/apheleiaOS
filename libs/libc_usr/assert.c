#include "stdio.h"


void __assert_fail(const char* assertion, const char* file, unsigned int line, const char* function) {
    fprintf(stderr, "%s:%u: %s: Assertion `%s' failed.\n", file, line, function, assertion);
    abort();
}
