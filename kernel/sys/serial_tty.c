#include "serial_tty.h"

#include <errno.h>
#include <sched/scheduler.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/usercopy.h>

void serial_tty_init(serial_tty_t *tty) {
    if (!tty || tty->ready) {
        return;
    }

    __termios_default_init(&tty->termios);

    tty->winsize.ws_col = 80;
    tty->winsize.ws_row = 25;
    tty->winsize.ws_xpixel = 0;
    tty->winsize.ws_ypixel = 0;
    tty->pgrp = 0;
    spinlock_init(&tty->lock);
    tty->ready = true;
}

static int get_termios(serial_tty_t *tty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    unsigned long flags = spin_lock_irqsave(&tty->lock);
    termios_t tos = tty->termios;
    spin_unlock_irqrestore(&tty->lock, flags);

    sched_thread_t *current = sched_current();
    if (!user_copy_to(current, args, &tos, sizeof(tos))) {
        return -EFAULT;
    }

    return 0;
}

static int set_termios(serial_tty_t *tty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    termios_t tos = { 0 };
    sched_thread_t *current = sched_current();
    if (!user_copy_from(current, &tos, args, sizeof(tos))) {
        return -EFAULT;
    }

    unsigned long flags = spin_lock_irqsave(&tty->lock);
    tty->termios = tos;
    spin_unlock_irqrestore(&tty->lock, flags);

    return 0;
}

static int get_winsize(serial_tty_t *tty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    unsigned long flags = spin_lock_irqsave(&tty->lock);
    winsize_t winsize = tty->winsize;
    spin_unlock_irqrestore(&tty->lock, flags);

    sched_thread_t *current = sched_current();
    if (!user_copy_to(current, args, &winsize, sizeof(winsize))) {
        return -EFAULT;
    }

    return 0;
}

static bool winsize_changed(const winsize_t *before, const winsize_t *after) {
    return before->ws_row != after->ws_row || before->ws_col != after->ws_col;
}

static int set_winsize(serial_tty_t *tty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    winsize_t winsize = { 0 };
    sched_thread_t *current = sched_current();
    if (!user_copy_from(current, &winsize, args, sizeof(winsize))) {
        return -EFAULT;
    }

    pid_t winch_pgrp = 0;
    unsigned long flags = spin_lock_irqsave(&tty->lock);
    winsize_t old = tty->winsize;
    tty->winsize = winsize;

    if (tty->pgrp > 0 && winsize_changed(&old, &winsize)) {
        winch_pgrp = tty->pgrp;
    }

    spin_unlock_irqrestore(&tty->lock, flags);

    if (winch_pgrp > 0) {
        sched_signal_send_pgrp(winch_pgrp, SIGWINCH);
    }

    return 0;
}

static int set_pgrp(serial_tty_t *tty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    pid_t requested = 0;
    sched_thread_t *current = sched_current();
    if (!user_copy_from(current, &requested, args, sizeof(requested))) {
        return -EFAULT;
    }

    if (requested <= 0) {
        return -EINVAL;
    }

    if (!current || !current->user_thread) {
        return -EPERM;
    }

    if (!sched_pgrp_in_session(requested, current->sid)) {
        return -EPERM;
    }

    unsigned long flags = spin_lock_irqsave(&tty->lock);
    tty->pgrp = requested;
    spin_unlock_irqrestore(&tty->lock, flags);

    return 0;
}

static int get_pgrp(serial_tty_t *tty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    unsigned long flags = spin_lock_irqsave(&tty->lock);
    pid_t pgrp = tty->pgrp;
    spin_unlock_irqrestore(&tty->lock, flags);

    sched_thread_t *current = sched_current();
    if (!user_copy_to(current, args, &pgrp, sizeof(pgrp))) {
        return -EFAULT;
    }

    return 0;
}

ssize_t serial_tty_ioctl(serial_tty_t *tty, u64 request, void *args) {
    if (!tty) {
        return -EINVAL;
    }

    serial_tty_init(tty);

    switch (request) {
    case TCGETS:
        return get_termios(tty, args);
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        return set_termios(tty, args);
    case TIOCGWINSZ:
        return get_winsize(tty, args);
    case TIOCSWINSZ:
        return set_winsize(tty, args);
    case TIOCSPGRP:
        return set_pgrp(tty, args);
    case TIOCGPGRP:
        return get_pgrp(tty, args);
    default:
        return -ENOTTY;
    }
}
