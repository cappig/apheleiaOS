#include "tty_input.h"

#include <data/ring.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/tty.h>
#include <x86/asm.h>

static ring_buffer_t* tty_buffers[TTY_SCREEN_COUNT] = {0};
static sched_wait_queue_t tty_wait[TTY_SCREEN_COUNT] = {0};
static bool tty_ready[TTY_SCREEN_COUNT] = {0};
static size_t tty_current_screen = TTY_CONSOLE;

static ring_buffer_t* _buffer(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT)
        return NULL;

    if (!tty_buffers[screen])
        tty_buffers[screen] = ring_buffer_create(TTY_INPUT_BUFFER_SIZE);

    if (!tty_buffers[screen]) {
        log_warn("tty: failed to allocate input buffer for screen %zu", screen);
        return NULL;
    }

    if (!tty_ready[screen]) {
        sched_wait_queue_init(&tty_wait[screen]);
        tty_ready[screen] = true;
    }

    return tty_buffers[screen];
}

void tty_input_init(void) {
    for (size_t i = 0; i < TTY_SCREEN_COUNT; i++)
        _buffer(i);
}

void tty_input_set_current(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT)
        return;

    tty_current_screen = screen;
    _buffer(screen);
}

void tty_input_push(char ch) {
    ring_buffer_t* buffer = _buffer(tty_current_screen);

    if (!buffer || ch == '\0')
        return;

    ring_buffer_push(buffer, (u8)ch);
    sched_wake_one(&tty_wait[tty_current_screen]);
}

ssize_t tty_input_read(size_t screen, void* buf, size_t len) {
    if (!buf || !len || screen >= TTY_SCREEN_COUNT)
        return 0;

    ring_buffer_t* buffer = _buffer(screen);
    if (!buffer)
        return -1;

    u8* out = buf;

    for (;;) {
        unsigned long flags = irq_save();
        size_t popped = ring_buffer_pop_array(buffer, out, len);

        if (popped) {
            irq_restore(flags);
            return (ssize_t)popped;
        }

        if (!sched_is_running()) {
            irq_restore(flags);
            continue;
        }

        sched_block_locked(&tty_wait[screen], flags);
    }
}
