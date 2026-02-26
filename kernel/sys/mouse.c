#include "mouse.h"

#include <arch/arch.h>
#include <base/types.h>
#include <data/ring.h>
#include <data/vector.h>
#include <errno.h>
#include <log/log.h>
#include <poll.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/devfs.h>
#include <sys/framebuffer.h>

#define MOUSE_DEV_BUFFER_SIZE 256
#define MOUSE_DEV_UID         0U
#define MOUSE_DEV_GID         45U
#define MOUSE_DEV_MODE        0644

typedef struct {
    const char *name;
} mouse_dev_t;

static vector_t *mice = NULL;
static ring_buffer_t *buffer = NULL;
static sched_wait_queue_t mouse_wait = {0};

static i32 mouse_x = 0;
static i32 mouse_y = 0;


static i32 _clamp_i32(i32 value, i32 min, i32 max) {
    if (value < min) {
        return min;
    }

    if (value > max) {
        return max;
    }

    return value;
}

static bool _has_events(void) {
    unsigned long irq_flags = arch_irq_save();
    bool has_events = buffer && !ring_buffer_is_empty(buffer);
    arch_irq_restore(irq_flags);

    return has_events;
}

static ssize_t
mouse_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
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

        sched_block(&mouse_wait);
    }
}

static short mouse_poll(vfs_node_t *node, short events, u32 flags) {
    (void)node;
    (void)flags;

    short revents = 0;

    if ((events & POLLIN) && _has_events()) {
        revents |= POLLIN;
    }

    return revents;
}

void mouse_handle_event(mouse_event event) {
    const framebuffer_info_t *fb = framebuffer_get_info();

    if (fb && fb->available && fb->width && fb->height) {
        mouse_x = _clamp_i32(mouse_x + event.delta_x, 0, (i32)fb->width - 1);
        mouse_y = _clamp_i32(mouse_y + event.delta_y, 0, (i32)fb->height - 1);
    } else {
        mouse_x += event.delta_x;
        mouse_y += event.delta_y;
    }

    if (buffer) {
        unsigned long irq_flags = arch_irq_save();
        ring_buffer_push_array(buffer, (u8 *)&event, sizeof(event));
        arch_irq_restore(irq_flags);
        sched_wake_all(&mouse_wait);
    }
}

static bool mouse_register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    if (!mice || !buffer) {
        log_warn("mouse state not initialized");
        return false;
    }

    vfs_interface_t *mouse_if = vfs_create_interface(mouse_read, NULL, NULL);
    if (!mouse_if) {
        log_warn("failed to allocate /dev interface");
        return false;
    }

    mouse_if->poll = mouse_poll;

    bool registered = devfs_register_node(
        dev_dir,
        "mouse",
        VFS_CHARDEV,
        MOUSE_DEV_MODE,
        mouse_if,
        NULL
    );

    if (!registered) {
        log_warn("failed to create /dev/mouse");
        return false;
    }

    vfs_node_t *mouse_node = vfs_lookup_from(dev_dir, "mouse");
    if (!mouse_node || !vfs_chown(mouse_node, MOUSE_DEV_UID, MOUSE_DEV_GID)) {
        log_warn("failed to set /dev/mouse ownership to root:input");
        return false;
    }

    return true;
}

bool mouse_init(void) {
    if (!devfs_register_device("mouse", mouse_register_devfs)) {
        log_warn("failed to register devfs init callback");
    }

    bool first_init = (mice == NULL || buffer == NULL);

    if (!mice) {
        mice = vec_create(sizeof(mouse_dev_t *));
    }

    if (!mice) {
        return false;
    }

    if (!buffer) {
        buffer = ring_buffer_create(MOUSE_DEV_BUFFER_SIZE);
    }

    if (!buffer) {
        return false;
    }

    if (!mouse_wait.list) {
        sched_wait_queue_init(&mouse_wait);
    }

    if (first_init) {
        const framebuffer_info_t *fb = framebuffer_get_info();

        if (fb && fb->available) {
            mouse_x = (i32)(fb->width / 2);
            mouse_y = (i32)(fb->height / 2);
        }
    }

    return true;
}

u8 mouse_register(const char *name) {
    if (!mice || !buffer) {
        if (!mouse_init()) {
            return 0;
        }
    }

    mouse_dev_t *mse = calloc(1, sizeof(mouse_dev_t));

    if (!mse) {
        return 0;
    }

    mse->name = strdup(name);

    if (!vec_push(mice, &mse)) {
        free((void *)mse->name);
        free(mse);
        return 0;
    }

    log_debug("registered %s", mse->name ? mse->name : "device");
    return (u8)(mice->size - 1);
}
