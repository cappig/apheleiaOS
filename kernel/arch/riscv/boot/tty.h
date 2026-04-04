#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define PRINTF_BUF_SIZE 256

int printf(const char *fmt, ...);
int puts(const char *str);

int serial_printf(const char *fmt, ...);
void serial_puts(const char *str);
void tty_set_uart_base(uintptr_t base);

NORETURN void panic(const char *msg);
