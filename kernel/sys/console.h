#pragma once

#include <base/types.h>
#include <data/ring.h>
#include <term/term.h>

#define KPRINTF_BUF_SIZE 256
#define CONSOLE_BUF_SIZE 8192


void conosle_init_buffer(void);
void console_set_serial(usize port);

void console_dump_buffer(void);

void kputs(const char* str);
void kputsn(const char* str, usize len);
int kprintf(char* fmt, ...) __attribute__((format(printf, 1, 2)));
