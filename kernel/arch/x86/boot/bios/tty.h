#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define PRINTF_BUF_SIZE 256

int printf(const char *fmt, ...);
int puts(const char *str);

void tty_disable_bios_output(void);

NORETURN void panic(const char *msg);

const char *boot_log_buffer(size_t *len, size_t *cap);
