#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define PRINTF_BUF_SIZE 256

int printf(char *fmt, ...);

void puts(const char *str);

int serial_printf(char *fmt, ...);

void serial_puts(const char *str);

NORETURN void panic(const char *msg);
