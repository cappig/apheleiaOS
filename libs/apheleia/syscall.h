#pragma once

#define SYSCALL(symbol, call, number) SYS_##symbol = number,

enum {
#include <apheleia/syscall.def>
    APHELEIA_SYSCALL_COUNT,
};

#undef SYSCALL
