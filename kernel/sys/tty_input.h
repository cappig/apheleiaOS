#pragma once

#include <base/types.h>
#include <sys/types.h>

#define TTY_INPUT_BUFFER_SIZE 1024

void tty_input_init(void);
void tty_input_set_current(size_t screen);
ssize_t tty_input_read(size_t screen, void* buf, size_t len);
void tty_input_push(char ch);
