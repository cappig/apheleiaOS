#pragma once

#include <base/types.h>
#include <boot/proto.h>
#include <data/ring.h>
#include <term/term.h>

#define KPRINTF_BUF_SIZE 256
#define CONSOLE_BUF_SIZE 8192


void kputs(const char* str);
void kputsn(const char* str, usize len);
int kprintf(char* fmt, ...) __attribute__((format(printf, 1, 2)));

void conosle_init(usize tty_index);

void print_motd(boot_handoff* handoff);
