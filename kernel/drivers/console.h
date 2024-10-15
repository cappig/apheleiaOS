#pragma once

#include <base/types.h>
#include <data/ring.h>
#include <term/term.h>

#include "vfs/fs.h"

#define KPRINTF_BUF_SIZE 256


void init_console(virtual_fs* vfs, terminal* term);

void console_set_write(term_putc_fn write_fn);
void console_set_serial(usize port);

void kputs(const char* str);
void kputsn(const char* str, usize len);
int kprintf(char* fmt, ...) __attribute__((format(printf, 1, 2)));
