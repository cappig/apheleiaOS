#pragma once

#include <base/attributes.h>

int printf(const char *fmt, ...);
NORETURN void panic(const char *msg);
