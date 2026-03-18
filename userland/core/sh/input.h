#pragma once

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

#define SH_INPUT_LINE_MAX 1024

void input_set_sigint_flag(volatile sig_atomic_t *flag);
void input_set_sigwinch_flag(volatile sig_atomic_t *flag);
void input_set_sigchld_flag(volatile sig_atomic_t *flag);
void input_set_sigchld_callback(void (*callback)(void));

void history_add(const char *line);
void history_print(void);

int read_line_interactive(
    const char *prompt,
    char *buf,
    size_t len,
    bool use_history
);
