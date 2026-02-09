#pragma once

#include <base/types.h>
#include <sys/types.h>
#include <termios.h>

#define TTY_INPUT_BUFFER_SIZE 1024

void tty_input_init(void);
void tty_input_set_current(size_t screen);
ssize_t tty_input_read(size_t screen, void* buf, size_t len);
void tty_input_push(char ch);

bool tty_input_get_termios(size_t screen, termios_t* out);
bool tty_input_set_termios(size_t screen, const termios_t* in, bool flush);
bool tty_input_get_winsize(size_t screen, winsize_t* out);
bool tty_input_set_winsize(size_t screen, const winsize_t* in);
void tty_input_flush(size_t screen);
