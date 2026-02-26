#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

enum {
    LESS_KEY_NONE = -1,
    LESS_KEY_UP = 1000,
    LESS_KEY_DOWN,
    LESS_KEY_PAGE_UP,
    LESS_KEY_PAGE_DOWN,
    LESS_KEY_HOME,
    LESS_KEY_END,
};

typedef struct {
    termios_t saved;
    bool saved_valid;
    bool raw;
    int saved_fd;
} less_tty_t;

void less_update_terminal_size(
    size_t *rows,
    size_t *cols,
    int input_fd,
    bool *changed
);
bool less_tty_enable_raw(less_tty_t *tty, int fd);
void less_tty_restore(less_tty_t *tty);
int less_tty_read_key(const less_tty_t *tty, int fd);
int less_open_controlling_input(void);
