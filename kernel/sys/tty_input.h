#pragma once

#include <base/types.h>
#include <sys/types.h>
#include <termios.h>

struct sched_wait_queue;

#define TTY_INPUT_BUFFER_SIZE 1024
#define TTY_TERMIOS_SET_NONE  0u
#define TTY_TERMIOS_SET_FLUSH 1u

void tty_input_init(void);
void tty_input_set_current(size_t screen);
ssize_t tty_input_read(size_t screen, void *buf, size_t len);
void tty_input_push(char ch);

bool tty_input_get_termios(size_t screen, termios_t *out);
bool tty_input_set_termios(size_t screen, const termios_t *in, u32 flags);
bool tty_input_get_winsize(size_t screen, winsize_t *out);
bool tty_input_set_winsize(size_t screen, const winsize_t *in);
bool tty_input_has_data(size_t screen);
void tty_input_flush(size_t screen);
struct sched_wait_queue *tty_input_wait_queue(size_t screen);
