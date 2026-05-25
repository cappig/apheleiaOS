#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define PRINTF_BUF_SIZE 256

int printf(const char *fmt, ...);
int puts(const char *str);

void tty_set_uart_base(uintptr_t base);

const char *boot_log_buffer(size_t *len, size_t *cap);

NORETURN void panic(const char *msg);
