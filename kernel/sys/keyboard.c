#include "keyboard.h"

#include <arch/arch.h>
#include <data/ring.h>
#include <data/vector.h>
#include <errno.h>
#include <log/log.h>
#include <poll.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/console.h>
#include <sys/devfs.h>
#include <sys/tty.h>
#include <sys/tty_input.h>

#define KBD_DEV_BUFFER_SIZE 256
#define KBD_DEV_UID         0U
#define KBD_DEV_GID         45U
#define KBD_DEV_MODE        0644

typedef struct {
    const char *name;

    bool shift;
    bool ctrl;
    bool alt;
    bool capslock;

    ascii_keymap *keymap;
} keyboard_dev_t;

static vector_t *kbds = NULL;
static ring_buffer_t *buffer = NULL;
static sched_wait_queue_t kbd_wait = {0};

static keyboard_dev_t *_get(size_t index) {
    return vec_at_ptr(kbds, index);
}

static bool _screen_captured(void) {
    ssize_t owner_screen = console_fb_owner_screen();

    if (owner_screen == TTY_NONE) {
        return false;
    }

    return tty_current_screen() == (size_t)owner_screen;
}

static bool _has_events(void) {
    unsigned long irq_flags = arch_irq_save();
    bool has_events = buffer && !ring_buffer_is_empty(buffer);
    arch_irq_restore(irq_flags);

    return has_events;
}

static void _update_modifiers(keyboard_dev_t *kbd, bool action, u8 code) {
    if (code == KBD_LEFT_SHIFT || code == KBD_RIGHT_SHIFT) {
        kbd->shift = (action == KEY_DOWN);
    }

    if (code == KBD_LEFT_CTRL || code == KBD_RIGHT_CTRL) {
        kbd->ctrl = (action == KEY_DOWN);
    }

    if (code == KBD_LEFT_ALT || code == KBD_RIGHT_ALT) {
        kbd->alt = (action == KEY_DOWN);
    }

    if ((code == KBD_CAPSLOCK) && (action == KEY_DOWN)) {
        kbd->capslock = !kbd->capslock;
    }
}

static bool _push_ansi_key(u8 code) {
    const char *seq = NULL;

    switch (code) {
    case KBD_UP:
        seq = "\x1b[A";
        break;
    case KBD_DOWN:
        seq = "\x1b[B";
        break;
    case KBD_RIGHT:
        seq = "\x1b[C";
        break;
    case KBD_LEFT:
        seq = "\x1b[D";
        break;
    case KBD_HOME:
        seq = "\x1b[H";
        break;
    case KBD_END:
        seq = "\x1b[F";
        break;
    case KBD_INSERT:
        seq = "\x1b[2~";
        break;
    case KBD_DELETE:
        seq = "\x1b[3~";
        break;
    case KBD_PAGEUP:
        seq = "\x1b[5~";
        break;
    case KBD_PAGEDOWN:
        seq = "\x1b[6~";
        break;
    default:
        return false;
    }

    while (*seq) {
        tty_input_push(*seq);
        seq++;
    }

    return true;
}

static ssize_t keyboard_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)offset;

    if (!buf || !buffer || !len) {
        return -EINVAL;
    }

    for (;;) {
        unsigned long irq_flags = arch_irq_save();
        size_t popped = ring_buffer_pop_array(buffer, buf, len);
        arch_irq_restore(irq_flags);

        if (popped) {
            return (ssize_t)popped;
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

        sched_block(&kbd_wait);
    }
}

static short keyboard_poll(vfs_node_t *node, short events, u32 flags) {
    (void)node;
    (void)flags;

    short revents = 0;

    if ((events & POLLIN) && _has_events()) {
        revents |= POLLIN;
    }

    return revents;
}

void keyboard_handle_key(key_event event) {
    if (!kbds || !buffer) {
        return;
    }

    keyboard_dev_t *kbd = _get(event.source);

    if (!kbd) {
        log_warn("keyboard input from unknown source");
        return;
    }

    bool action = (event.type & KEY_ACTION) != 0;
    unsigned long irq_flags = arch_irq_save();
    ring_buffer_push_array(buffer, (u8 *)&event, sizeof(event));
    arch_irq_restore(irq_flags);
    sched_wake_all(&kbd_wait);

    _update_modifiers(kbd, action, event.code);

    if (!action) {
        return;
    }

    if (kbd->alt) {
        if (event.code == KBD_F1) {
            tty_set_current(TTY_CONSOLE);
            return;
        }

        if (event.code >= KBD_F2 && event.code <= (KBD_F2 + TTY_COUNT - 1)) {
            size_t index = (size_t)(event.code - KBD_F2);

            if (index < TTY_COUNT) {
                tty_set_current(TTY_USER_TO_SCREEN(index));
            }

            return;
        }
    }

    if (_screen_captured()) {
        return;
    }

    if (_push_ansi_key(event.code)) {
        return;
    }

    bool is_alpha = (event.code >= KBD_A && event.code <= KBD_Z);
    bool shift = kbd->shift ^ (kbd->capslock && is_alpha);
    char ch = kbd_to_ascii(event, kbd->keymap, shift);

    if (!ch) {
        return;
    }

    if (kbd->ctrl) {
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 1);
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 1);
        }
    }

    tty_input_push(ch);
}

static bool keyboard_register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    if (!kbds || !buffer) {
        log_warn("keyboard state not initialized");
        return false;
    }

    vfs_interface_t *kbd_if = vfs_create_interface(keyboard_read, NULL, NULL);
    if (!kbd_if) {
        log_warn("failed to allocate /dev interface");
        return false;
    }

    kbd_if->poll = keyboard_poll;

    bool registered = devfs_register_node(
        dev_dir,
        "keyboard",
        VFS_CHARDEV,
        KBD_DEV_MODE,
        kbd_if,
        NULL
    );

    if (!registered) {
        log_warn("failed to create /dev/keyboard");
        return false;
    }

    vfs_node_t *kbd_node = vfs_lookup_from(dev_dir, "keyboard");
    if (!kbd_node || !vfs_chown(kbd_node, KBD_DEV_UID, KBD_DEV_GID)) {
        log_warn("failed to set /dev/keyboard ownership to root:input");
        return false;
    }

    return true;
}

bool keyboard_init(void) {
    if (!devfs_register_device("keyboard", keyboard_register_devfs)) {
        log_warn("failed to register devfs init callback");
    }

    if (!kbds) {
        kbds = vec_create(sizeof(keyboard_dev_t *));
    }

    if (!kbds) {
        return false;
    }

    if (!buffer) {
        buffer = ring_buffer_create(KBD_DEV_BUFFER_SIZE);
    }

    if (!buffer) {
        return false;
    }

    if (!kbd_wait.list) {
        sched_wait_queue_init(&kbd_wait);
    }

    return true;
}

u8 keyboard_register(const char *name, ascii_keymap *keymap) {
    if (!kbds || !buffer) {
        if (!keyboard_init()) {
            return 0;
        }
    }

    keyboard_dev_t *kbd = calloc(1, sizeof(keyboard_dev_t));

    if (!kbd) {
        return 0;
    }

    kbd->name = strdup(name);
    kbd->keymap = keymap ? keymap : &us_keymap;

    if (!vec_push(kbds, &kbd)) {
        free((void *)kbd->name);
        free(kbd);
        return 0;
    }

    log_debug("registered %s", kbd->name ? kbd->name : "device");
    return (u8)(kbds->size - 1);
}
