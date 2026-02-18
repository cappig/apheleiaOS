#include "input.h"

#include <arch/arch.h>
#include <errno.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <string.h>
#include <sys/console.h>
#include <sys/devfs.h>
#include <sys/tty.h>

#define INPUT_QUEUE_CAPACITY 256

typedef struct {
    input_event_t events[INPUT_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    u8 mouse_buttons[256];
    sched_wait_queue_t wait_queue;
    bool ready;
} input_state_t;

static input_state_t input_state = {0};


bool input_capture_screen(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT) {
        return false;
    }

    ssize_t owner_screen = console_fb_owner_screen();

    if (owner_screen == TTY_NONE) {
        return false;
    }

    return screen == (size_t)owner_screen;
}

static u64 _timestamp_ms(void) {
    u32 hz = arch_timer_hz();
    if (!hz) {
        return 0;
    }

    return (arch_timer_ticks() * 1000ULL) / hz;
}

static bool _has_events(void) {
    unsigned long irq_flags = arch_irq_save();
    bool has_events = input_state.count > 0;
    arch_irq_restore(irq_flags);

    return has_events;
}

static void _push_event(const input_event_t *event) {
    if (!event || !input_state.ready) {
        return;
    }

    unsigned long irq_flags = arch_irq_save();

    if (input_state.count == INPUT_QUEUE_CAPACITY) {
        input_state.head = (input_state.head + 1) % INPUT_QUEUE_CAPACITY;
        input_state.count--;
    }

    input_state.events[input_state.tail] = *event;
    input_state.tail = (input_state.tail + 1) % INPUT_QUEUE_CAPACITY;
    input_state.count++;

    arch_irq_restore(irq_flags);

    sched_wake_all(&input_state.wait_queue);
}

static bool input_register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    if (!input_state.ready) {
        log_warn("input state not initialized");
        return false;
    }

    vfs_interface_t *input_if = vfs_create_interface(input_read, NULL, NULL);
    if (!input_if) {
        log_warn("failed to allocate /dev interface");
        return false;
    }

    input_if->poll = input_poll;

    if (!devfs_register_node(dev_dir, "input", VFS_CHARDEV, 0666, input_if, NULL)) {
        log_warn("failed to create /dev/input");
        return false;
    }

    return true;
}

bool input_init(void) {
    if (!devfs_register_device("input", input_register_devfs)) {
        log_warn("failed to register devfs init callback");
    }

    if (input_state.ready) {
        return true;
    }

    memset(&input_state, 0, sizeof(input_state));
    sched_wait_queue_init(&input_state.wait_queue);
    input_state.ready = true;

    return true;
}

void input_push_key_event(const key_event *event, bool shift, bool ctrl, bool alt, bool capslock) {
    if (!event || !input_state.ready) {
        return;
    }

    if (!input_capture_screen(tty_current_screen())) {
        return;
    }

    input_event_t input = {0};

    input.timestamp_ms = _timestamp_ms();
    input.type = INPUT_EVENT_KEY;
    input.source = event->source;
    input.keycode = event->code;
    input.action = (event->type & KEY_ACTION) ? 1 : 0;

    if (shift) {
        input.modifiers |= INPUT_MOD_SHIFT;
    }

    if (ctrl) {
        input.modifiers |= INPUT_MOD_CTRL;
    }

    if (alt) {
        input.modifiers |= INPUT_MOD_ALT;
    }

    if (capslock) {
        input.modifiers |= INPUT_MOD_CAPS;
    }

    _push_event(&input);
}

void input_push_mouse_event(const mouse_event *event) {
    if (!event || !input_state.ready) {
        return;
    }

    if (!input_capture_screen(tty_current_screen())) {
        return;
    }

    if (event->delta_x || event->delta_y) {
        input_event_t move = {0};

        move.timestamp_ms = _timestamp_ms();
        move.type = INPUT_EVENT_MOUSE_MOVE;
        move.source = event->source;
        move.dx = event->delta_x;
        move.dy = event->delta_y;
        move.buttons = event->buttons;

        _push_event(&move);
    }

    u8 source = event->source;
    u8 prev_buttons = input_state.mouse_buttons[source];

    if (prev_buttons == event->buttons) {
        return;
    }

    input_state.mouse_buttons[source] = event->buttons;

    input_event_t buttons = {0};

    buttons.timestamp_ms = _timestamp_ms();
    buttons.type = INPUT_EVENT_MOUSE_BUTTON;
    buttons.source = source;
    buttons.buttons = event->buttons;

    _push_event(&buttons);
}

ssize_t input_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)offset;

    if (!buf) {
        return -EFAULT;
    }

    if (len < sizeof(input_event_t)) {
        return -EINVAL;
    }

    size_t max_events = len / sizeof(input_event_t);
    if (!max_events) {
        return -EINVAL;
    }

    for (;;) {
        size_t copied = 0;
        input_event_t *out_events = (input_event_t *)buf;

        unsigned long irq_flags = arch_irq_save();

        while (copied < max_events && input_state.count > 0) {
            out_events[copied] = input_state.events[input_state.head];
            input_state.head = (input_state.head + 1) % INPUT_QUEUE_CAPACITY;
            input_state.count--;
            copied++;
        }

        arch_irq_restore(irq_flags);

        if (copied) {
            return (ssize_t)(copied * sizeof(input_event_t));
        }

        if (flags & VFS_NONBLOCK) {
            return -EAGAIN;
        }

        if (!sched_is_running()) {
            continue;
        }

        sched_thread_t *current = sched_current();

        if (current && sched_signal_has_pending(current)) {
            return -EINTR;
        }

        sched_block(&input_state.wait_queue);
    }
}

short input_poll(vfs_node_t *node, short events, u32 flags) {
    (void)node;
    (void)flags;

    short revents = 0;

    if ((events & POLLIN) && _has_events()) {
        revents |= POLLIN;
    }

    return revents;
}
