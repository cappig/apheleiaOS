#include "ws.h"

#include <arch/arch.h>
#include <errno.h>
#include <gui/input.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/devfs.h>
#include <sys/ioctl.h>
#include <sys/lock.h>
#include <sys/stats.h>

#define WS_MGR_QUEUE_INIT_CAP 64
#define WS_EV_QUEUE_INIT_CAP  64
#define WS_QUEUE_MAX_CAP      4096
#define WS_MAX_FB_BYTES       (16U * 1024U * 1024U)
#define WS_WINDOW_INIT_CAP    16

typedef struct {
    bool allocated;
    pid_t owner_pid;
    char title[WS_TITLE_MAX];
    i32 x;
    i32 y;
    u32 width;
    u32 height;
    u32 stride;
    u32 z;
    u32 flags;
    u8 *fb;
    u32 fb_store_width;
    u32 fb_store_height;
    u32 fb_store_stride;
    size_t fb_store_size;
    size_t fb_size;
    size_t fb_capacity;
    volatile int fb_io_lock;
    u32 io_refs;
    bool pending_free;
    bool pending_notify_manager;
    bool mgr_dirty_pending;
    u32 mgr_dirty_x;
    u32 mgr_dirty_y;
    u32 mgr_dirty_width;
    u32 mgr_dirty_height;
    ws_input_event_t *ev_queue;
    size_t ev_capacity;
    size_t ev_head;
    size_t ev_tail;
    size_t ev_count;
    sched_wait_queue_t ev_wait;
} ws_window_t;

typedef struct {
    bool ready;
    pid_t manager_pid;
    ws_window_t *windows;
    size_t window_capacity;
    ws_event_t *mgr_queue;
    size_t mgr_capacity;
    size_t mgr_head;
    size_t mgr_tail;
    size_t mgr_count;
    sched_wait_queue_t mgr_wait;
    vfs_node_t *ws_dir;
    vfs_interface_t *ws_fb_if;
    vfs_interface_t *ws_ev_if;
} ws_state_t;

static ws_state_t ws_state = {0};
static volatile int ws_lock = 0;

static void *_slot_priv_encode(u32 id) {
    return (void *)(uintptr_t)((u64)id + 1ULL);
}

static bool _slot_priv_decode(void *priv, u32 *id_out) {
    if (!priv || !id_out) {
        return false;
    }

    uintptr_t raw = (uintptr_t)priv;
    if (!raw) {
        return false;
    }

    *id_out = (u32)(raw - 1);
    return true;
}


static pid_t _current_pid(void) {
    sched_thread_t *current = sched_current();
    if (!current) {
        return 0;
    }

    return current->pid;
}

static bool _pid_alive(pid_t pid) {
    return pid > 0 && sched_find_thread(pid);
}

static bool _is_manager(pid_t pid) {
    return pid > 0 && ws_state.manager_pid == pid;
}

static bool _window_access(const ws_window_t *window, pid_t pid) {
    if (!window || !window->allocated || pid <= 0) {
        return false;
    }

    return window->owner_pid == pid || _is_manager(pid);
}

static ws_window_t *_window_slot(u32 id) {
    if (!ws_state.windows || id >= ws_state.window_capacity) {
        return NULL;
    }

    return &ws_state.windows[id];
}

static bool _windows_reserve(size_t needed) {
    if (ws_state.window_capacity >= needed) {
        return true;
    }

    size_t new_capacity = ws_state.window_capacity ? ws_state.window_capacity : WS_WINDOW_INIT_CAP;
    while (new_capacity < needed) {
        size_t grown = new_capacity * 2;
        if (grown <= new_capacity) {
            return false;
        }
        new_capacity = grown;
    }

    ws_window_t *windows = calloc(new_capacity, sizeof(*windows));
    if (!windows) {
        return false;
    }

    if (ws_state.windows && ws_state.window_capacity) {
        memcpy(windows, ws_state.windows, ws_state.window_capacity * sizeof(*windows));
        free(ws_state.windows);
    }

    for (size_t i = ws_state.window_capacity; i < new_capacity; i++) {
        sched_wait_queue_init(&windows[i].ev_wait);
    }

    ws_state.windows = windows;
    ws_state.window_capacity = new_capacity;
    return true;
}

static bool _mgr_queue_reserve(size_t needed) {
    if (ws_state.mgr_capacity >= needed) {
        return true;
    }

    size_t new_capacity = ws_state.mgr_capacity ? ws_state.mgr_capacity : WS_MGR_QUEUE_INIT_CAP;
    while (new_capacity < needed) {
        if (new_capacity >= WS_QUEUE_MAX_CAP) {
            return false;
        }

        size_t grown = new_capacity * 2;
        if (grown <= new_capacity) {
            return false;
        }

        new_capacity = grown;
    }

    ws_event_t *queue = calloc(new_capacity, sizeof(*queue));
    if (!queue) {
        return false;
    }

    if (ws_state.mgr_queue && ws_state.mgr_count && ws_state.mgr_capacity) {
        for (size_t i = 0; i < ws_state.mgr_count; i++) {
            size_t index = (ws_state.mgr_head + i) % ws_state.mgr_capacity;
            queue[i] = ws_state.mgr_queue[index];
        }
    }

    free(ws_state.mgr_queue);
    ws_state.mgr_queue = queue;
    ws_state.mgr_capacity = new_capacity;
    ws_state.mgr_head = 0;
    ws_state.mgr_tail = ws_state.mgr_count;

    return true;
}

static bool _mgr_queue_push(const ws_event_t *event) {
    if (!event) {
        return false;
    }

    if (ws_state.mgr_count == ws_state.mgr_capacity) {
        if (!_mgr_queue_reserve(ws_state.mgr_count + 1)) {
            if (!ws_state.mgr_capacity) {
                return false;
            }

            ws_event_t dropped = ws_state.mgr_queue[ws_state.mgr_head];
            if (dropped.type == WS_EVT_WINDOW_DIRTY) {
                ws_window_t *window = _window_slot(dropped.id);
                if (window) {
                    window->mgr_dirty_pending = false;
                    window->mgr_dirty_x = 0;
                    window->mgr_dirty_y = 0;
                    window->mgr_dirty_width = 0;
                    window->mgr_dirty_height = 0;
                }
            }

            ws_state.mgr_head = (ws_state.mgr_head + 1) % ws_state.mgr_capacity;
            ws_state.mgr_count--;
        }
    }

    if (!ws_state.mgr_capacity || !ws_state.mgr_queue) {
        return false;
    }

    ws_state.mgr_queue[ws_state.mgr_tail] = *event;
    ws_state.mgr_tail = (ws_state.mgr_tail + 1) % ws_state.mgr_capacity;
    ws_state.mgr_count++;

    return true;
}

static void _window_dirty_clear_pending(ws_window_t *window) {
    if (!window) {
        return;
    }

    window->mgr_dirty_pending = false;
    window->mgr_dirty_x = 0;
    window->mgr_dirty_y = 0;
    window->mgr_dirty_width = 0;
    window->mgr_dirty_height = 0;
}

static void _window_dirty_merge_pending(ws_window_t *window, u32 x, u32 y, u32 width, u32 height) {
    if (!window || !width || !height) {
        return;
    }

    if (!window->mgr_dirty_pending || !window->mgr_dirty_width || !window->mgr_dirty_height) {
        window->mgr_dirty_pending = true;
        window->mgr_dirty_x = x;
        window->mgr_dirty_y = y;
        window->mgr_dirty_width = width;
        window->mgr_dirty_height = height;
        return;
    }

    u32 x0 = window->mgr_dirty_x < x ? window->mgr_dirty_x : x;
    u32 y0 = window->mgr_dirty_y < y ? window->mgr_dirty_y : y;
    u32 x1 = (window->mgr_dirty_x + window->mgr_dirty_width) > (x + width)
                 ? (window->mgr_dirty_x + window->mgr_dirty_width)
                 : (x + width);
    u32 y1 = (window->mgr_dirty_y + window->mgr_dirty_height) > (y + height)
                 ? (window->mgr_dirty_y + window->mgr_dirty_height)
                 : (y + height);

    window->mgr_dirty_x = x0;
    window->mgr_dirty_y = y0;
    window->mgr_dirty_width = x1 - x0;
    window->mgr_dirty_height = y1 - y0;
}

static void _mgr_queue_drop_window_dirty(u32 id) {
    if (!ws_state.mgr_count || !ws_state.mgr_queue || !ws_state.mgr_capacity) {
        return;
    }

    size_t kept = 0;
    size_t write = ws_state.mgr_head;

    for (size_t i = 0; i < ws_state.mgr_count; i++) {
        size_t index = (ws_state.mgr_head + i) % ws_state.mgr_capacity;
        ws_event_t event = ws_state.mgr_queue[index];
        if (event.type == WS_EVT_WINDOW_DIRTY && event.id == id) {
            continue;
        }

        ws_state.mgr_queue[write] = event;
        write = (write + 1) % ws_state.mgr_capacity;
        kept++;
    }

    ws_state.mgr_count = kept;
    ws_state.mgr_tail = (ws_state.mgr_head + kept) % ws_state.mgr_capacity;
}

static bool _window_ev_reserve(ws_window_t *window, size_t needed) {
    if (!window) {
        return false;
    }

    if (window->ev_capacity >= needed) {
        return true;
    }

    size_t new_capacity = window->ev_capacity ? window->ev_capacity : WS_EV_QUEUE_INIT_CAP;
    while (new_capacity < needed) {
        if (new_capacity >= WS_QUEUE_MAX_CAP) {
            return false;
        }

        size_t grown = new_capacity * 2;
        if (grown <= new_capacity) {
            return false;
        }

        new_capacity = grown;
    }

    ws_input_event_t *queue = calloc(new_capacity, sizeof(*queue));
    if (!queue) {
        return false;
    }

    if (window->ev_queue && window->ev_count && window->ev_capacity) {
        for (size_t i = 0; i < window->ev_count; i++) {
            size_t index = (window->ev_head + i) % window->ev_capacity;
            queue[i] = window->ev_queue[index];
        }
    }

    free(window->ev_queue);
    window->ev_queue = queue;
    window->ev_capacity = new_capacity;
    window->ev_head = 0;
    window->ev_tail = window->ev_count;

    return true;
}

static bool _window_ev_push(ws_window_t *window, const ws_input_event_t *event) {
    if (!window || !event) {
        return false;
    }

    if (event->type == INPUT_EVENT_WINDOW_RESIZE && window->ev_count && window->ev_capacity &&
        window->ev_queue) {
        // Coalesce to the newest queued resize event so geometry stays in sync.
        for (size_t i = 0; i < window->ev_count; i++) {
            size_t rel = window->ev_count - 1 - i;
            size_t index = (window->ev_head + rel) % window->ev_capacity;

            if (window->ev_queue[index].type == INPUT_EVENT_WINDOW_RESIZE) {
                window->ev_queue[index] = *event;
                return true;
            }
        }
    }

    if (window->ev_count == window->ev_capacity) {
        if (!_window_ev_reserve(window, window->ev_count + 1)) {
            if (!window->ev_capacity) {
                return false;
            }

            // If we cannot grow the queue, drop the oldest event and keep the
            // newest one. This is especially important for resize events.
            window->ev_head = (window->ev_head + 1) % window->ev_capacity;
            window->ev_count--;
        }
    }

    if (!window->ev_capacity || !window->ev_queue) {
        return false;
    }

    window->ev_queue[window->ev_tail] = *event;
    window->ev_tail = (window->ev_tail + 1) % window->ev_capacity;
    window->ev_count++;

    return true;
}

static void _queue_manager_event(u32 type, u32 id, const ws_window_t *window) {
    if (!ws_state.manager_pid) {
        return;
    }

    ws_event_t event = {0};
    event.type = type;
    event.id = id;

    if (window) {
        event.owner_pid = window->owner_pid;
        event.x = window->x;
        event.y = window->y;
        event.width = window->width;
        event.height = window->height;
        strncpy(event.title, window->title, sizeof(event.title) - 1);
    }

    if (!_mgr_queue_push(&event)) {
        return;
    }

    sched_wake_all(&ws_state.mgr_wait);
}

static void _queue_manager_dirty_event(
    u32 id,
    ws_window_t *window,
    u32 x,
    u32 y,
    u32 width,
    u32 height
) {
    if (!window || !ws_state.manager_pid || !width || !height) {
        return;
    }

    if (x >= window->width || y >= window->height) {
        return;
    }

    if (x + width > window->width) {
        width = window->width - x;
    }

    if (y + height > window->height) {
        height = window->height - y;
    }

    if (!width || !height) {
        return;
    }

    bool had_pending = window->mgr_dirty_pending;
    _window_dirty_merge_pending(window, x, y, width, height);

    if (had_pending) {
        sched_wake_all(&ws_state.mgr_wait);
        return;
    }

    ws_event_t event = {0};
    event.type = WS_EVT_WINDOW_DIRTY;
    event.id = id;
    event.owner_pid = window->owner_pid;
    event.x = (i32)window->mgr_dirty_x;
    event.y = (i32)window->mgr_dirty_y;
    event.width = window->mgr_dirty_width;
    event.height = window->mgr_dirty_height;

    if (!_mgr_queue_push(&event)) {
        _window_dirty_clear_pending(window);
        return;
    }

    sched_wake_all(&ws_state.mgr_wait);
}

static void _queue_manager_dirty_write(u32 id, ws_window_t *window, size_t offset, size_t len) {
    if (!window || !len || window->width == 0) {
        return;
    }

    size_t start_pixel = offset / sizeof(u32);
    size_t end_pixel = (offset + len - 1) / sizeof(u32);

    u32 y0 = (u32)(start_pixel / window->width);
    u32 x0 = (u32)(start_pixel % window->width);
    u32 y1 = (u32)(end_pixel / window->width);
    u32 x1 = (u32)(end_pixel % window->width);

    if (y0 == y1) {
        _queue_manager_dirty_event(id, window, x0, y0, x1 - x0 + 1, 1);
        return;
    }
    _queue_manager_dirty_event(id, window, 0, y0, window->width, y1 - y0 + 1);
}

static void _finalize_window_free(u32 id, bool notify_manager) {
    ws_window_t *window = _window_slot(id);
    if (!window) {
        return;
    }

    if (!window->allocated) {
        return;
    }

    _window_dirty_clear_pending(window);
    _mgr_queue_drop_window_dirty(id);

    ws_window_t snapshot = *window;
    sched_wake_all(&window->ev_wait);

    free(window->fb);
    free(window->ev_queue);
    linked_list_t *ev_wait_list = window->ev_wait.list;

    memset(window, 0, sizeof(*window));
    window->ev_wait.list = ev_wait_list;

    if (!window->ev_wait.list) {
        sched_wait_queue_init(&window->ev_wait);
    }

    if (notify_manager) {
        _queue_manager_event(WS_EVT_WINDOW_CLOSED, id, &snapshot);
    }
}

static void _free_window(u32 id, bool notify_manager) {
    ws_window_t *window = _window_slot(id);
    if (!window) {
        return;
    }

    if (!window->allocated) {
        return;
    }

    if (window->io_refs) {
        window->pending_free = true;
        window->pending_notify_manager = window->pending_notify_manager || notify_manager;
        sched_wake_all(&window->ev_wait);
        return;
    }

    _finalize_window_free(id, notify_manager);
}

static void _window_acquire_io(ws_window_t *window) {
    if (!window) {
        return;
    }

    window->io_refs++;
}

static void _window_release_io(u32 id, ws_window_t *window) {
    if (!window || !window->io_refs) {
        return;
    }

    window->io_refs--;

    if (!window->io_refs && window->pending_free) {
        bool notify_manager = window->pending_notify_manager;
        _finalize_window_free(id, notify_manager);
    }
}

static void _clear_focus(void) {
    for (size_t i = 0; i < ws_state.window_capacity; i++) {
        ws_window_t *window = &ws_state.windows[i];

        window->flags &= ~WS_WINDOW_FOCUSED;
    }
}

static void _drop_manager_and_close_windows(pid_t manager_pid) {
    pid_t *owners = NULL;
    size_t owner_capacity = 0;
    size_t owner_count = 0;

    ws_state.manager_pid = 0;

    for (u32 i = 0; i < ws_state.window_capacity; i++) {
        ws_window_t *window = &ws_state.windows[i];
        if (!window->allocated) {
            continue;
        }

        pid_t owner_pid = window->owner_pid;
        _free_window(i, false);

        if (owner_pid <= 0 || owner_pid == manager_pid) {
            continue;
        }

        bool seen = false;
        for (size_t j = 0; j < owner_count; j++) {
            if (owners[j] == owner_pid) {
                seen = true;
                break;
            }
        }

        if (!seen) {
            if (owner_count == owner_capacity) {
                size_t new_capacity = owner_capacity ? owner_capacity * 2 : 8;
                pid_t *grown = calloc(new_capacity, sizeof(*grown));
                if (!grown) {
                    continue;
                }

                if (owners && owner_count) {
                    memcpy(grown, owners, owner_count * sizeof(*grown));
                }
                free(owners);
                owners = grown;
                owner_capacity = new_capacity;
            }

            owners[owner_count++] = owner_pid;
        }
    }

    ws_state.mgr_head = 0;
    ws_state.mgr_tail = 0;
    ws_state.mgr_count = 0;
    _clear_focus();

    for (size_t i = 0; i < owner_count; i++) {
        sched_signal_send_pid(owners[i], SIGHUP);
    }

    free(owners);
}

static void _reap_dead_owners(void) {
    if (ws_state.manager_pid && !_pid_alive(ws_state.manager_pid)) {
        _drop_manager_and_close_windows(ws_state.manager_pid);
    }

    for (u32 i = 0; i < ws_state.window_capacity; i++) {
        ws_window_t *window = &ws_state.windows[i];
        if (!window->allocated) {
            continue;
        }

        if (_pid_alive(window->owner_pid)) {
            continue;
        }

        _free_window(i, true);
    }
}

static void _cmd_fill_from_window(ws_cmd_t *cmd, u32 id) {
    if (!cmd) {
        return;
    }

    cmd->id = id;
    cmd->x = 0;
    cmd->y = 0;
    cmd->width = 0;
    cmd->height = 0;
    cmd->stride = 0;
    cmd->flags = 0;

    ws_window_t *window = _window_slot(id);
    if (!window || !window->allocated) {
        return;
    }

    cmd->x = window->x;
    cmd->y = window->y;
    cmd->width = window->width;
    cmd->height = window->height;
    cmd->stride = window->stride;
    cmd->flags = window->flags;
}

static int _window_lookup(u32 id, pid_t caller_pid, ws_window_t **out) {
    ws_window_t *window = _window_slot(id);
    if (!window) {
        return -EINVAL;
    }

    if (!window->allocated) {
        return -ENOENT;
    }

    if (window->pending_free) {
        return -ENOENT;
    }

    if (!_window_access(window, caller_pid)) {
        return -EPERM;
    }

    if (out) {
        *out = window;
    }
    return 0;
}

static size_t _copy_len(size_t size, size_t offset, size_t len) {
    if (offset >= size) {
        return 0;
    }

    size_t copy_len = size - offset;
    if (copy_len > len) {
        copy_len = len;
    }

    return copy_len;
}

static void _copy_from_store(const ws_window_t *window, void *dst, size_t offset, size_t len) {
    if (!window || !window->fb || !dst || !len || !window->stride || !window->fb_store_stride) {
        return;
    }

    size_t view_stride = (size_t)window->stride;
    size_t store_stride = (size_t)window->fb_store_stride;

    if (view_stride == store_stride) {
        memcpy(dst, window->fb + offset, len);
        return;
    }

    u8 *out = dst;
    size_t done = 0;
    size_t remaining = len;

    while (remaining > 0) {
        size_t row = offset / view_stride;
        size_t col = offset % view_stride;
        if (row >= window->height) {
            break;
        }

        size_t chunk = view_stride - col;
        if (chunk > remaining) {
            chunk = remaining;
        }

        size_t src_off = row * store_stride + col;
        memcpy(out + done, window->fb + src_off, chunk);

        done += chunk;
        offset += chunk;
        remaining -= chunk;
    }
}

static void _copy_to_store(ws_window_t *window, const void *src, size_t offset, size_t len) {
    if (!window || !window->fb || !src || !len || !window->stride || !window->fb_store_stride) {
        return;
    }

    size_t view_stride = (size_t)window->stride;
    size_t store_stride = (size_t)window->fb_store_stride;

    if (view_stride == store_stride) {
        memcpy(window->fb + offset, src, len);
        return;
    }

    const u8 *in = src;
    size_t done = 0;
    size_t remaining = len;

    while (remaining > 0) {
        size_t row = offset / view_stride;
        size_t col = offset % view_stride;
        if (row >= window->height) {
            break;
        }

        size_t chunk = view_stride - col;
        if (chunk > remaining) {
            chunk = remaining;
        }

        size_t dst_off = row * store_stride + col;
        memcpy(window->fb + dst_off, in + done, chunk);

        done += chunk;
        offset += chunk;
        remaining -= chunk;
    }
}

static bool _ensure_slot_nodes(u32 id) {
    if (!ws_state.ws_dir || !ws_state.ws_fb_if || !ws_state.ws_ev_if) {
        return false;
    }

    char slot_name[16];
    snprintf(slot_name, sizeof(slot_name), "%u", id);

    vfs_node_t *slot = devfs_register_dir(ws_state.ws_dir, slot_name, 0755);
    if (!slot) {
        return false;
    }

    void *priv = _slot_priv_encode(id);

    if (!devfs_register_node(slot, "fb", VFS_CHARDEV, 0666, ws_state.ws_fb_if, priv)) {
        return false;
    }

    if (!devfs_register_node(slot, "ev", VFS_CHARDEV, 0666, ws_state.ws_ev_if, priv)) {
        return false;
    }

    return true;
}

static int _handle_alloc(pid_t caller_pid, ws_cmd_t *cmd) {
    if (!cmd) {
        return -EINVAL;
    }

    if (!ws_state.manager_pid || !_pid_alive(ws_state.manager_pid)) {
        return -EPIPE;
    }

    if (!cmd->width || !cmd->height) {
        return -EINVAL;
    }

    u64 stride = (u64)cmd->width * 4ULL;
    u64 fb_size_u64 = stride * (u64)cmd->height;

    if (!stride || fb_size_u64 > WS_MAX_FB_BYTES) {
        return -EINVAL;
    }

    u32 free_id = (u32)ws_state.window_capacity;
    for (u32 i = 0; i < ws_state.window_capacity; i++) {
        ws_window_t *candidate = _window_slot(i);
        if (!candidate) {
            break;
        }

        if (!candidate->allocated) {
            free_id = i;
            break;
        }
    }

    if (!_windows_reserve((size_t)free_id + 1)) {
        return -ENOMEM;
    }

    ws_window_t *window = _window_slot(free_id);
    if (!window) {
        return -ENOMEM;
    }

    if (!_ensure_slot_nodes(free_id)) {
        return -EIO;
    }

    linked_list_t *ev_wait_list = window->ev_wait.list;

    memset(window, 0, sizeof(*window));

    window->ev_wait.list = ev_wait_list;
    if (!window->ev_wait.list) {
        sched_wait_queue_init(&window->ev_wait);
    }

    window->fb = calloc(1, (size_t)fb_size_u64);
    if (!window->fb) {
        return -ENOMEM;
    }

    if (!_window_ev_reserve(window, WS_EV_QUEUE_INIT_CAP)) {
        free(window->fb);
        window->fb = NULL;
        return -ENOMEM;
    }

    window->allocated = true;
    window->owner_pid = cmd->pid > 0 && _is_manager(caller_pid) ? cmd->pid : caller_pid;
    window->x = cmd->x;
    window->y = cmd->y;
    window->width = cmd->width;
    window->height = cmd->height;
    window->stride = (u32)stride;
    window->fb_store_width = cmd->width;
    window->fb_store_height = cmd->height;
    window->fb_store_stride = (u32)stride;
    window->fb_store_size = (size_t)fb_size_u64;
    window->z = free_id;
    window->flags = cmd->flags | WS_WINDOW_MAPPED;
    window->fb_size = (size_t)fb_size_u64;
    window->fb_capacity = (size_t)fb_size_u64;

    strncpy(window->title, cmd->title, sizeof(window->title) - 1);

    _queue_manager_event(WS_EVT_WINDOW_NEW, free_id, window);

    _cmd_fill_from_window(cmd, free_id);
    return 0;
}

static int _handle_free(pid_t caller_pid, ws_cmd_t *cmd) {
    if (!cmd) {
        return -EINVAL;
    }

    int status = _window_lookup(cmd->id, caller_pid, NULL);
    if (status) {
        return status;
    }

    u32 id = cmd->id;
    _free_window(id, true);

    _cmd_fill_from_window(cmd, id);
    return 0;
}

static int _handle_query(pid_t caller_pid, ws_cmd_t *cmd) {
    if (!cmd) {
        return -EINVAL;
    }

    int status = _window_lookup(cmd->id, caller_pid, NULL);
    if (status) {
        return status;
    }

    _cmd_fill_from_window(cmd, cmd->id);
    return 0;
}

static int _handle_set_size(u32 id, ws_window_t *window, ws_cmd_t *cmd) {
    if (!window || !cmd) {
        return -EINVAL;
    }

    if (!cmd->width || !cmd->height) {
        return -EINVAL;
    }

    if (cmd->width == window->width && cmd->height == window->height) {
        _cmd_fill_from_window(cmd, id);
        return 0;
    }

    u64 stride_u64 = (u64)cmd->width * 4ULL;
    u64 fb_size_u64 = stride_u64 * (u64)cmd->height;
    if (!stride_u64 || fb_size_u64 > WS_MAX_FB_BYTES) {
        return -EINVAL;
    }

    if (window->io_refs) {
        return -EAGAIN;
    }

    size_t view_size = (size_t)fb_size_u64;

    u32 old_store_width = window->fb_store_width;
    u32 old_store_height = window->fb_store_height;
    size_t old_store_stride = (size_t)window->fb_store_stride;
    size_t old_store_size = window->fb_store_size;

    u32 need_store_width = old_store_width;
    if (cmd->width > need_store_width) {
        need_store_width = cmd->width;
    }
    if (!need_store_width) {
        need_store_width = cmd->width;
    }

    u32 need_store_height = old_store_height;
    if (cmd->height > need_store_height) {
        need_store_height = cmd->height;
    }
    if (!need_store_height) {
        need_store_height = cmd->height;
    }

    u64 need_store_stride_u64 = (u64)need_store_width * 4ULL;
    u64 need_store_size_u64 = need_store_stride_u64 * (u64)need_store_height;
    if (!need_store_stride_u64 || need_store_size_u64 > WS_MAX_FB_BYTES) {
        return -EINVAL;
    }

    size_t need_store_stride = (size_t)need_store_stride_u64;
    size_t need_store_size = (size_t)need_store_size_u64;

    bool stride_change = old_store_stride != need_store_stride;
    bool need_alloc = !window->fb || stride_change || window->fb_capacity < need_store_size;
    u8 *resized_fb = NULL;
    size_t resized_capacity = 0;

    if (need_alloc) {
        size_t new_capacity = window->fb_capacity ? window->fb_capacity : old_store_size;
        if (!new_capacity) {
            new_capacity = need_store_size;
        }

        while (new_capacity < need_store_size) {
            size_t grown = new_capacity * 2;
            if (grown <= new_capacity) {
                new_capacity = need_store_size;
                break;
            }
            new_capacity = grown;
        }

        if (new_capacity < need_store_size || new_capacity > WS_MAX_FB_BYTES) {
            return -ENOMEM;
        }

        resized_fb = calloc(1, new_capacity);
        if (!resized_fb) {
            return -ENOMEM;
        }

        if (window->fb && old_store_width && old_store_height && old_store_stride) {
            size_t row_bytes = (size_t)old_store_width * sizeof(u32);
            for (u32 row = 0; row < old_store_height; row++) {
                const u8 *src = window->fb + ((size_t)row * old_store_stride);
                u8 *dst = resized_fb + ((size_t)row * need_store_stride);
                memcpy(dst, src, row_bytes);
            }
        }

        resized_capacity = new_capacity;
    }

    ws_input_event_t resize_event = {0};
    resize_event.type = INPUT_EVENT_WINDOW_RESIZE;
    resize_event.width = cmd->width;
    resize_event.height = cmd->height;
    resize_event.stride = (u32)stride_u64;

    if (!_window_ev_push(window, &resize_event)) {
        if (resized_fb) {
            free(resized_fb);
        }
        return -ENOMEM;
    }

    if (need_alloc) {
        u8 *old_fb = window->fb;
        window->fb = resized_fb;
        window->fb_capacity = resized_capacity;
        free(old_fb);
    }

    if (window->fb && old_store_width && old_store_height && need_store_width > old_store_width) {
        for (u32 row = 0; row < old_store_height; row++) {
            u8 *row_base = window->fb + ((size_t)row * need_store_stride);
            const u32 *edge = (const u32 *)(row_base + ((size_t)(old_store_width - 1) * sizeof(u32)));
            u32 fill = *edge;

            u32 *dst = (u32 *)(row_base + ((size_t)old_store_width * sizeof(u32)));
            for (u32 col = old_store_width; col < need_store_width; col++) {
                *dst++ = fill;
            }
        }
    }

    if (window->fb && need_store_height > old_store_height) {
        if (old_store_height > 0) {
            const u8 *src_row = window->fb + ((size_t)(old_store_height - 1) * need_store_stride);
            for (u32 row = old_store_height; row < need_store_height; row++) {
                u8 *dst_row = window->fb + ((size_t)row * need_store_stride);
                memcpy(dst_row, src_row, need_store_stride);
            }
        } else {
            u8 *dst = window->fb + ((size_t)old_store_height * need_store_stride);
            size_t grow_bytes = (size_t)(need_store_height - old_store_height) * need_store_stride;
            memset(dst, 0, grow_bytes);
        }
    }

    window->fb_store_width = need_store_width;
    window->fb_store_height = need_store_height;
    window->fb_store_stride = (u32)need_store_stride_u64;
    window->fb_store_size = need_store_size;
    window->width = cmd->width;
    window->height = cmd->height;
    window->stride = (u32)stride_u64;
    window->fb_size = view_size;

    // Make resize immediately visible to the compositor even for apps that
    // do not repaint on resize (e.g. simple event-loop demos).
    _queue_manager_dirty_event(cmd->id, window, 0, 0, window->width, window->height);

    sched_wake_all(&window->ev_wait);

    _cmd_fill_from_window(cmd, id);
    return 0;
}

static int _handle_manager_op(pid_t caller_pid, u64 request, ws_cmd_t *cmd) {
    if (!cmd) {
        return -EINVAL;
    }

    switch (request) {
    case WSIOC_CLAIM_MANAGER:
        if (ws_state.manager_pid && ws_state.manager_pid != caller_pid) {
            return -EBUSY;
        }

        ws_state.manager_pid = caller_pid;
        _cmd_fill_from_window(cmd, cmd->id);

        return 0;
    case WSIOC_RELEASE_MANAGER:
        if (!_is_manager(caller_pid)) {
            return -EPERM;
        }

        _drop_manager_and_close_windows(caller_pid);
        _cmd_fill_from_window(cmd, cmd->id);

        return 0;
    default:
        break;
    }

    if (!_is_manager(caller_pid)) {
        return -EPERM;
    }

    ws_window_t *window = _window_slot(cmd->id);
    if (!window) {
        return -EINVAL;
    }

    if (!window->allocated) {
        return -ENOENT;
    }

    switch (request) {
    case WSIOC_SET_FOCUS:
        _clear_focus();
        window->flags |= WS_WINDOW_FOCUSED;
        _cmd_fill_from_window(cmd, cmd->id);
        return 0;
    case WSIOC_SET_POS:
        window->x = cmd->x;
        window->y = cmd->y;
        _cmd_fill_from_window(cmd, cmd->id);
        return 0;
    case WSIOC_SET_SIZE:
        return _handle_set_size(cmd->id, window, cmd);
    case WSIOC_SET_Z:
        window->z = cmd->flags;
        _cmd_fill_from_window(cmd, cmd->id);
        return 0;
    case WSIOC_SEND_INPUT:
        if (!_window_ev_push(window, &cmd->input)) {
            return -ENOMEM;
        }

        sched_wake_all(&window->ev_wait);
        _cmd_fill_from_window(cmd, cmd->id);

        return 0;
    case WSIOC_CLOSE:
        pid_t owner_pid = window->owner_pid;
        _free_window(cmd->id, true);

        if (owner_pid > 0 && owner_pid != caller_pid) {
            sched_signal_send_pid(owner_pid, SIGHUP);
        }

        _cmd_fill_from_window(cmd, cmd->id);

        return 0;
    default:
        return -ENOTTY;
    }
}

static bool _ws_state_init(void) {
    if (ws_state.ready) {
        return true;
    }

    memset(&ws_state, 0, sizeof(ws_state));

    if (!_windows_reserve(WS_WINDOW_INIT_CAP)) {
        return false;
    }

    ws_state.mgr_queue = calloc(WS_MGR_QUEUE_INIT_CAP, sizeof(ws_event_t));
    if (!ws_state.mgr_queue) {
        free(ws_state.windows);
        ws_state.windows = NULL;
        ws_state.window_capacity = 0;
        return false;
    }

    ws_state.mgr_capacity = WS_MGR_QUEUE_INIT_CAP;
    sched_wait_queue_init(&ws_state.mgr_wait);

    ws_state.ready = true;

    return true;
}

static ssize_t _dev_wsctl_ioctl(vfs_node_t *node, u64 request, void *args) {
    return ws_ctl_ioctl(node, request, args);
}

static short _dev_wsctl_poll(vfs_node_t *node, short events, u32 flags) {
    return ws_ctl_poll(node, events, flags);
}

static ssize_t _dev_wsmgr_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    return ws_mgr_read(node, buf, offset, len, flags);
}

static short _dev_wsmgr_poll(vfs_node_t *node, short events, u32 flags) {
    return ws_mgr_poll(node, events, flags);
}

static ssize_t _dev_ws_fb_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    u32 id = 0;
    if (!node || !_slot_priv_decode(node->private, &id)) {
        return -EINVAL;
    }

    return ws_fb_read(id, buf, offset, len, flags);
}

static ssize_t _dev_ws_fb_write(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    u32 id = 0;
    if (!node || !_slot_priv_decode(node->private, &id)) {
        return -EINVAL;
    }

    return ws_fb_write(id, buf, offset, len, flags);
}

static short _dev_ws_fb_poll(vfs_node_t *node, short events, u32 flags) {
    u32 id = 0;
    if (!node || !_slot_priv_decode(node->private, &id)) {
        return POLLNVAL;
    }

    return ws_fb_poll(id, events, flags);
}

static ssize_t _dev_ws_ev_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    u32 id = 0;
    if (!node || !_slot_priv_decode(node->private, &id)) {
        return -EINVAL;
    }

    return ws_ev_read(id, buf, offset, len, flags);
}

static short _dev_ws_ev_poll(vfs_node_t *node, short events, u32 flags) {
    u32 id = 0;
    if (!node || !_slot_priv_decode(node->private, &id)) {
        return POLLNVAL;
    }

    return ws_ev_poll(id, events, flags);
}

static bool ws_register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    if (!_ws_state_init()) {
        log_warn("WS init failed");
        return false;
    }

    bool ok = true;

    vfs_interface_t *wsctl_if = vfs_create_interface(NULL, NULL, NULL);
    if (!wsctl_if) {
        log_warn("failed to allocate /dev/wsctl interface");
        ok = false;
    } else {
        wsctl_if->ioctl = _dev_wsctl_ioctl;
        wsctl_if->poll = _dev_wsctl_poll;
        if (!devfs_register_node(dev_dir, "wsctl", VFS_CHARDEV, 0666, wsctl_if, NULL)) {
            log_warn("failed to create /dev/wsctl");
            ok = false;
        }
    }

    vfs_interface_t *wsmgr_if = vfs_create_interface(_dev_wsmgr_read, NULL, NULL);
    if (!wsmgr_if) {
        log_warn("failed to allocate /dev/wsmgr interface");
        ok = false;
    } else {
        wsmgr_if->poll = _dev_wsmgr_poll;
        if (!devfs_register_node(dev_dir, "wsmgr", VFS_CHARDEV, 0666, wsmgr_if, NULL)) {
            log_warn("failed to create /dev/wsmgr");
            ok = false;
        }
    }

    ws_state.ws_dir = devfs_register_dir(dev_dir, "ws", 0755);
    if (!ws_state.ws_dir) {
        log_warn("failed to create /dev/ws");
        return false;
    }

    ws_state.ws_fb_if = vfs_create_interface(_dev_ws_fb_read, _dev_ws_fb_write, NULL);
    ws_state.ws_ev_if = vfs_create_interface(_dev_ws_ev_read, NULL, NULL);

    if (!ws_state.ws_fb_if || !ws_state.ws_ev_if) {
        log_warn("failed to allocate window interfaces");
        return false;
    }

    ws_state.ws_fb_if->poll = _dev_ws_fb_poll;
    ws_state.ws_ev_if->poll = _dev_ws_ev_poll;

    return ok;
}

bool ws_init(void) {
    if (!devfs_register_device("ws", ws_register_devfs)) {
        log_warn("failed to register devfs init callback");
    }

    return _ws_state_init();
}

ssize_t ws_ctl_ioctl(vfs_node_t *node, u64 request, void *args) {
    (void)node;

    if (!args) {
        return -EINVAL;
    }

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0) {
        return -EPERM;
    }

    ws_cmd_t *cmd = args;

    int status = 0;

    lock(&ws_lock);
    _reap_dead_owners();

    switch (request) {
    case WSIOC_ALLOC:
        status = _handle_alloc(caller_pid, cmd);
        break;
    case WSIOC_FREE:
        status = _handle_free(caller_pid, cmd);
        break;
    case WSIOC_QUERY:
        status = _handle_query(caller_pid, cmd);
        break;
    case WSIOC_CLAIM_MANAGER:
    case WSIOC_RELEASE_MANAGER:
    case WSIOC_SET_FOCUS:
    case WSIOC_SET_POS:
    case WSIOC_SET_SIZE:
    case WSIOC_SET_Z:
    case WSIOC_SEND_INPUT:
    case WSIOC_CLOSE:
        status = _handle_manager_op(caller_pid, request, cmd);
        break;
    default:
        status = -ENOTTY;
        break;
    }

    unlock(&ws_lock);
    return status;
}

short ws_ctl_poll(vfs_node_t *node, short events, u32 flags) {
    (void)node;
    (void)flags;

    short revents = 0;

    if (events & POLLOUT) {
        revents |= POLLOUT;
    }

    return revents;
}

ssize_t ws_mgr_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)offset;

    if (!buf || len < sizeof(ws_event_t)) {
        return -EINVAL;
    }

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0) {
        return -EPERM;
    }

    for (;;) {
        lock(&ws_lock);
        _reap_dead_owners();

        if (!_is_manager(caller_pid)) {
            unlock(&ws_lock);
            return -EPERM;
        }

        if (ws_state.mgr_count > 0) {
            if (!ws_state.mgr_queue || !ws_state.mgr_capacity) {
                ws_state.mgr_head = 0;
                ws_state.mgr_tail = 0;
                ws_state.mgr_count = 0;
                unlock(&ws_lock);
                return -EIO;
            }

            ws_event_t event = ws_state.mgr_queue[ws_state.mgr_head];
            ws_state.mgr_head = (ws_state.mgr_head + 1) % ws_state.mgr_capacity;
            ws_state.mgr_count--;

            if (event.type == WS_EVT_WINDOW_DIRTY) {
                ws_window_t *window = _window_slot(event.id);
                if (!window || !window->allocated || window->owner_pid != event.owner_pid ||
                    !window->mgr_dirty_pending || !window->mgr_dirty_width || !window->mgr_dirty_height) {
                    unlock(&ws_lock);
                    continue;
                }

                event.x = (i32)window->mgr_dirty_x;
                event.y = (i32)window->mgr_dirty_y;
                event.width = window->mgr_dirty_width;
                event.height = window->mgr_dirty_height;
                _window_dirty_clear_pending(window);
            }
            unlock(&ws_lock);

            memcpy(buf, &event, sizeof(event));
            return (ssize_t)sizeof(event);
        }

        unlock(&ws_lock);

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

        sched_block(&ws_state.mgr_wait);
    }
}

short ws_mgr_poll(vfs_node_t *node, short events, u32 flags) {
    (void)node;
    (void)flags;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0) {
        return POLLNVAL;
    }

    lock(&ws_lock);
    _reap_dead_owners();

    if (!_is_manager(caller_pid)) {
        unlock(&ws_lock);
        return POLLNVAL;
    }

    short revents = 0;
    if ((events & POLLIN) && ws_state.mgr_count) {
        revents |= POLLIN;
    }
    if (events & POLLOUT) {
        revents |= POLLOUT;
    }

    unlock(&ws_lock);
    return revents;
}

ssize_t ws_fb_read(u32 id, void *buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!buf) {
        return -EINVAL;
    }

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0) {
        return -EPERM;
    }

    lock(&ws_lock);

    ws_window_t *window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status) {
        unlock(&ws_lock);
        return status;
    }

    size_t copy_len = _copy_len(window->fb_size, offset, len);
    if (!copy_len) {
        unlock(&ws_lock);
        return VFS_EOF;
    }

    _window_acquire_io(window);
    unlock(&ws_lock);

    lock(&window->fb_io_lock);
    _copy_from_store(window, buf, offset, copy_len);
    unlock(&window->fb_io_lock);

    lock(&ws_lock);
    _window_release_io(id, window);
    unlock(&ws_lock);

    return (ssize_t)copy_len;
}

ssize_t ws_fb_write(u32 id, const void *buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!buf) {
        return -EINVAL;
    }

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0) {
        return -EPERM;
    }

    lock(&ws_lock);

    ws_window_t *window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status) {
        unlock(&ws_lock);
        return status;
    }

    size_t copy_len = _copy_len(window->fb_size, offset, len);
    if (!copy_len) {
        unlock(&ws_lock);
        return VFS_EOF;
    }

    if (copy_len != len) {
        unlock(&ws_lock);
        return -EAGAIN;
    }

    _window_acquire_io(window);
    unlock(&ws_lock);

    lock(&window->fb_io_lock);
    _copy_to_store(window, buf, offset, copy_len);
    unlock(&window->fb_io_lock);

    stats_add_ws_fb_write_bytes((u64)copy_len);
    stats_add_wm_dirty_pixels((u64)(copy_len / 4));

    lock(&ws_lock);
    _queue_manager_dirty_write(id, window, offset, copy_len);

    _window_release_io(id, window);
    unlock(&ws_lock);

    return (ssize_t)copy_len;
}

void ws_notify_screen_active(void) {
    lock(&ws_lock);

    for (u32 i = 0; i < ws_state.window_capacity; i++) {
        ws_window_t *window = _window_slot(i);
        if (!window || !window->allocated) {
            continue;
        }

        _queue_manager_dirty_event(i, window, 0, 0, window->width, window->height);
    }

    unlock(&ws_lock);
}

short ws_fb_poll(u32 id, short events, u32 flags) {
    (void)flags;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0) {
        return POLLNVAL;
    }

    lock(&ws_lock);

    ws_window_t *window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status == -ENOENT) {
        unlock(&ws_lock);
        return POLLHUP;
    }

    if (status) {
        unlock(&ws_lock);
        return POLLNVAL;
    }

    short revents = 0;

    if (events & POLLIN) {
        revents |= POLLIN;
    }

    if (events & POLLOUT) {
        revents |= POLLOUT;
    }

    unlock(&ws_lock);
    return revents;
}

ssize_t ws_ev_read(u32 id, void *buf, size_t offset, size_t len, u32 flags) {
    (void)offset;

    if (!buf) {
        return -EINVAL;
    }

    if (len < sizeof(ws_input_event_t)) {
        return -EINVAL;
    }

    size_t max_events = len / sizeof(ws_input_event_t);
    if (!max_events) {
        return -EINVAL;
    }

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0) {
        return -EPERM;
    }

    for (;;) {
        size_t copied = 0;
        ws_input_event_t *out_events = (ws_input_event_t *)buf;

        lock(&ws_lock);
        _reap_dead_owners();

        ws_window_t *window = NULL;
        int status = _window_lookup(id, caller_pid, &window);
        if (status) {
            unlock(&ws_lock);
            return status;
        }

        if (window->ev_count && (!window->ev_queue || !window->ev_capacity)) {
            unlock(&ws_lock);
            return -EIO;
        }

        while (copied < max_events && window->ev_count > 0) {
            out_events[copied] = window->ev_queue[window->ev_head];
            window->ev_head = (window->ev_head + 1) % window->ev_capacity;
            window->ev_count--;
            copied++;
        }

        if (copied) {
            unlock(&ws_lock);
            return (ssize_t)(copied * sizeof(ws_input_event_t));
        }

        if (flags & VFS_NONBLOCK) {
            unlock(&ws_lock);
            return -EAGAIN;
        }

        if (!sched_is_running()) {
            unlock(&ws_lock);
            continue;
        }

        sched_thread_t *current = sched_current();
        if (current && sched_signal_has_pending(current)) {
            unlock(&ws_lock);
            return -EINTR;
        }

        // Hold io_ref while blocking so the window (and its wait queue list)
        // cannot be finalized underneath us
        _window_acquire_io(window);
        sched_wait_queue_t *wait_queue = &window->ev_wait;
        unlock(&ws_lock);

        sched_block(wait_queue);

        lock(&ws_lock);
        _window_release_io(id, window);
        unlock(&ws_lock);
    }
}

short ws_ev_poll(u32 id, short events, u32 flags) {
    (void)flags;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0) {
        return POLLNVAL;
    }

    lock(&ws_lock);
    _reap_dead_owners();

    ws_window_t *window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status == -ENOENT) {
        unlock(&ws_lock);
        return POLLHUP;
    }

    if (status) {
        unlock(&ws_lock);
        return POLLNVAL;
    }

    short revents = 0;
    if ((events & POLLIN) && window->ev_count) {
        revents |= POLLIN;
    }

    unlock(&ws_lock);
    return revents;
}
