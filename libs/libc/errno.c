#include "errno.h"

static int _errno_internal = 0;

int* _errno_impl(void) {
    return &_errno_internal;
}
