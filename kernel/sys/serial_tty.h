#pragma once

#include <base/types.h>
#include <sys/lock.h>
#include <sys/types.h>
#include <termios.h>

typedef struct {
    termios_t termios;
    winsize_t winsize;
    pid_t pgrp;
    spinlock_t lock;
    bool ready;
} serial_tty_t;

void serial_tty_init(serial_tty_t *tty);
ssize_t serial_tty_ioctl(serial_tty_t *tty, u64 request, void *args);
