#pragma once

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

#define SH_INPUT_LINE_MAX 1024

void sh_input_set_sigint_flag(volatile sig_atomic_t* flag);

void sh_history_add(const char* line);
void sh_history_print(void);

int sh_read_line_interactive(const char* prompt, char* buf, size_t len, bool use_history);
