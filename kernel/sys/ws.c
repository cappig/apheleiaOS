#include "ws.h"

#include <arch/arch.h>
#include <data/ring.h>
#include <data/vector.h>
#include <errno.h>
#include <gui/input.h>
#include <limits.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/config.h>
#include <sys/devfs.h>
#include <sys/framebuffer.h>
#include <sys/ioctl.h>
#include <sys/lock.h>
#include <sys/time.h>
#include <sys/usercopy.h>

#define WS_DEV_UID 0U
#define WS_DEV_GID 46U

typedef struct {
    u32 id;
    bool allocated;
    pid_t owner_pid;
    char title[WS_TITLE_MAX];

    i32 x;
    i32 y;
    u32 width;
    u32 height;
    u32 stride;

    u32 io_width;
    u32 io_height;
    u32 io_stride;

    u32 z;
    u32 flags;

    u8 *fb;
    u32 fb_store_width;
    u32 fb_store_height;
    u32 fb_store_stride;
    size_t fb_store_size;
    size_t fb_size;
    size_t io_fb_size;
    size_t fb_capacity;
    mutex_t fb_io_lock;
    u32 io_refs;
    bool pending_free;

    bool notify_mgr_pending;
    bool pending_ev_wake;

    bool mgr_dirty_pending;
    u32 mgr_dirty_x;
    u32 mgr_dirty_y;
    u32 mgr_dirty_width;
    u32 mgr_dirty_height;

    ring_queue_t *ev_queue;
    size_t ev_high_water;
    sched_wait_queue_t ev_wait;
} ws_window_t;

typedef struct {
    bool ready;
    pid_t manager_pid;
    vector_t *windows;
    ring_queue_t *mgr_queue;
    size_t mgr_high_water;
    size_t ev_high_water;
    bool pending_mgr_wake;
    sched_wait_queue_t mgr_wait;
    vfs_node_t *ws_dir;
    vfs_interface_t *wsctl_if;
    vfs_interface_t *wsmgr_if;
    vfs_interface_t *ws_fb_if;
    vfs_interface_t *ws_ev_if;
    sched_thread_t *reaper_thread;
    mutex_t lock;
} ws_state_t;

typedef struct {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
} ws_rect_t;

typedef struct {
    u32 id;
    pid_t pid;
    void *buf;
    size_t offset;
    size_t len;
    u32 flags;
} ws_io_t;

typedef struct {
    void *buf;
    size_t offset;
    size_t len;
    u32 view_height;
    u32 view_stride;
    bool write;
} ws_copy_t;

static ws_state_t ws_state = {
    .lock = MUTEX_INIT,
};

static inline bool _event_is_lossy(const ws_input_event_t *event) {
    if (!event) {
        return false;
    }

    return event->type == INPUT_EVENT_MOUSE_MOVE || event->type == INPUT_EVENT_MOUSE_WHEEL;
}

static u32 _rect_end(u32 start, u32 size) {
    if (size > (u32)-1 - start) {
        return (u32)-1;
    }

    return start + size;
}

static bool _set_ws_owner(vfs_node_t *node, const char *path) {
    if (!node || vfs_chown(node, WS_DEV_UID, WS_DEV_GID) < 0) {
        log_warn("failed to set %s ownership to root:ws", path ? path : "node");
        return false;
    }

    return true;
}

static void *_slot_priv_encode(u32 id) {
    return (void *)(uintptr_t)((u64)id + 1ULL);
}

static bool _slot_priv_decode(void *priv, u32 *id_out) {
    if (!priv || !id_out) {
        return false;
    }

    uintptr_t raw = (uintptr_t)priv;
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

static inline void _defer_manager_wake(void) {
    ws_state.pending_mgr_wake = true;
}

static inline void _defer_window_wake(ws_window_t *window) {
    if (window) {
        window->pending_ev_wake = true;
    }
}

static void _window_slot_reinit(ws_window_t *window) {
    if (!window) {
        return;
    }

    linked_list_t *ev_wait_list = window->ev_wait.list;
    u32 id = window->id;

    mutex_destroy(&window->fb_io_lock);
    free(window->fb);
    ring_queue_destroy(window->ev_queue);

    memset(window, 0, sizeof(*window));
    window->id = id;
    window->ev_wait.list = ev_wait_list;
    mutex_init(&window->fb_io_lock);
    sched_waitq_init(&window->ev_wait);
    sched_waitq_set_poll(&window->ev_wait, true);
}

static ws_window_t *_window_slot_create(u32 id) {
    ws_window_t *window = calloc(1, sizeof(*window));
    if (!window) {
        return NULL;
    }

    window->id = id;
    _window_slot_reinit(window);
    return window;
}

static ws_window_t *_window_slot(u32 id) {
    if (!ws_state.windows || id >= vec_size(ws_state.windows)) {
        return NULL;
    }

    return (ws_window_t *)vec_at_ptr(ws_state.windows, id);
}

static void flush_wakes(void) {
    vector_t *window_wakes = vec_create(sizeof(sched_wait_queue_t *));

    for (;;) {
        bool wake_manager = false;
        bool have_one_wake = false;
        sched_wait_queue_t *one_wake = NULL;

        if (window_wakes) {
            vec_clear(window_wakes);
        }

        mutex_lock(&ws_state.lock);

        if (ws_state.pending_mgr_wake) {
            ws_state.pending_mgr_wake = false;
            wake_manager = true;
        }

        if (ws_state.windows) {
            for (size_t i = 0; i < vec_size(ws_state.windows); i++) {
                ws_window_t *window = _window_slot((u32)i);
                if (!window) {
                    continue;
                }

                if (!window->pending_ev_wake) {
                    continue;
                }

                if (window_wakes) {
                    sched_wait_queue_t *wait_queue = &window->ev_wait;
                    if (vec_push(window_wakes, &wait_queue)) {
                        window->pending_ev_wake = false;
                        continue;
                    }
                }

                window->pending_ev_wake = false;
                one_wake = &window->ev_wait;
                have_one_wake = true;
                break;
            }
        }

        mutex_unlock(&ws_state.lock);

        size_t wake_count = window_wakes ? window_wakes->size : 0;
        if (!wake_manager && !wake_count && !have_one_wake) {
            vec_destroy(window_wakes);
            return;
        }

        if (wake_manager) {
            // manager queue has a single consumer; waking one avoids herd wakeups
            sched_wake_one(&ws_state.mgr_wait);
        }

        for (size_t i = 0; i < wake_count; i++) {
            sched_wait_queue_t **wait_queue = vec_at(window_wakes, i);
            if (wait_queue && *wait_queue) {
                sched_wake_all(*wait_queue);
            }
        }

        if (have_one_wake && one_wake) {
            sched_wake_all(one_wake);
        }
    }
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

static bool _windows_reserve(size_t needed) {
    if (!ws_state.windows) {
        ws_state.windows = vec_create_sized(WS_WINDOW_INIT_CAP, sizeof(ws_window_t *));
        if (!ws_state.windows) {
            return false;
        }
    }

    if (vec_capacity(ws_state.windows) < needed) {
        if (!vec_reserve(ws_state.windows, needed)) {
            return false;
        }
    }

    if (vec_size(ws_state.windows) >= needed) {
        return true;
    }

    size_t old_count = vec_size(ws_state.windows);

    if (!vec_resize(ws_state.windows, needed)) {
        return false;
    }

    for (size_t i = old_count; i < needed; i++) {
        ws_window_t *window = _window_slot_create((u32)i);

        if (!window) {
            for (size_t j = old_count; j < i; j++) {
                ws_window_t **slot = vec_at(ws_state.windows, j);

                if (slot && *slot) {
                    ring_queue_destroy((*slot)->ev_queue);
                    free((*slot)->fb);
                    sched_waitq_destroy(&(*slot)->ev_wait);
                    free(*slot);
                    *slot = NULL;
                }
            }

            vec_resize(ws_state.windows, old_count);
            return false;
        }

        if (!vec_set(ws_state.windows, i, &window)) {
            for (size_t j = old_count; j < i; j++) {
                ws_window_t **slot = vec_at(ws_state.windows, j);

                if (slot && *slot) {
                    ring_queue_destroy((*slot)->ev_queue);
                    free((*slot)->fb);
                    sched_waitq_destroy(&(*slot)->ev_wait);
                    free(*slot);
                    *slot = NULL;
                }
            }

            sched_waitq_destroy(&window->ev_wait);
            free(window);

            vec_resize(ws_state.windows, old_count);
            return false;
        }
    }

    return true;
}

static void _dirty_clear(ws_window_t *window) {
    if (!window) {
        return;
    }

    window->mgr_dirty_pending = false;
    window->mgr_dirty_x = 0;
    window->mgr_dirty_y = 0;
    window->mgr_dirty_width = 0;
    window->mgr_dirty_height = 0;
}

static bool _mgr_queue_push(const ws_event_t *event) {
    if (!event || !ws_state.mgr_queue) {
        return false;
    }

    ring_queue_t *q = ws_state.mgr_queue;

    if (ring_queue_count(q) == ring_queue_capacity(q)) {
        if (!ring_queue_reserve(q, ring_queue_count(q) + 1)) {
            ws_event_t dropped;
            if (!ring_queue_pop(q, &dropped)) {
                return false;
            }
            if (dropped.type == WS_EVT_WINDOW_DIRTY) {
                ws_window_t *window = _window_slot(dropped.id);
                if (window) {
                    _dirty_clear(window);
                }
            }
        }
    }

    if (!ring_queue_push(q, event)) {
        return false;
    }

    size_t count = ring_queue_count(q);
    if (count > ws_state.mgr_high_water) {
        ws_state.mgr_high_water = count;
    }

    return true;
}

static void _dirty_merge(ws_window_t *window, const ws_rect_t *rect) {
    if (!window || !rect || !rect->width || !rect->height) {
        return;
    }

    // the manager only needs one dirty event; keep folding the damage into it
    if (!window->mgr_dirty_pending || !window->mgr_dirty_width || !window->mgr_dirty_height) {
        window->mgr_dirty_pending = true;
        window->mgr_dirty_x = rect->x;
        window->mgr_dirty_y = rect->y;
        window->mgr_dirty_width = rect->width;
        window->mgr_dirty_height = rect->height;
        return;
    }

    u32 x0 = window->mgr_dirty_x < rect->x ? window->mgr_dirty_x : rect->x;
    u32 y0 = window->mgr_dirty_y < rect->y ? window->mgr_dirty_y : rect->y;

    u32 dx = _rect_end(window->mgr_dirty_x, window->mgr_dirty_width);
    u32 new_dx = _rect_end(rect->x, rect->width);
    u32 x1 = dx > new_dx ? dx : new_dx;

    u32 dy = _rect_end(window->mgr_dirty_y, window->mgr_dirty_height);
    u32 new_dy = _rect_end(rect->y, rect->height);
    u32 y1 = dy > new_dy ? dy : new_dy;

    window->mgr_dirty_x = x0;
    window->mgr_dirty_y = y0;
    window->mgr_dirty_width = x1 - x0;
    window->mgr_dirty_height = y1 - y0;
}

static void _mgr_drop_dirty(u32 id) {
    ring_queue_t *q = ws_state.mgr_queue;
    if (!q) {
        return;
    }

    for (size_t i = 0; i < ring_queue_count(q);) {
        ws_event_t *ev = ring_queue_at(q, i);
        if (ev && ev->type == WS_EVT_WINDOW_DIRTY && ev->id == id) {
            ring_queue_remove_at(q, i);
        } else {
            i++;
        }
    }
}

static bool _window_ev_push(ws_window_t *window, const ws_input_event_t *event) {
    if (!window || !event) {
        return false;
    }

    ring_queue_t *q = window->ev_queue;
    if (!q) {
        return false;
    }

    if (ring_queue_count(q) == ring_queue_capacity(q)) {
        if (_event_is_lossy(event)) {
            return true;
        }

        if (!ring_queue_reserve(q, ring_queue_count(q) + 1)) {
            bool dropped = false;
            for (size_t i = 0; i < ring_queue_count(q); i++) {
                ws_input_event_t *queued = ring_queue_at(q, i);
                if (queued && _event_is_lossy(queued)) {
                    ring_queue_remove_at(q, i);
                    dropped = true;
                    break;
                }
            }
            if (!dropped) {
                // preserve progress for the newest control event even if it means
                // discarding the oldest stale event in a saturated queue
                ring_queue_drop_head(q);
            }
        }
    }

    if (!ring_queue_push(q, event)) {
        return false;
    }

    size_t count = ring_queue_count(q);
    if (count > window->ev_high_water) {
        window->ev_high_water = count;

        if (window->ev_high_water > ws_state.ev_high_water) {
            ws_state.ev_high_water = window->ev_high_water;
        }
    }

    return true;
}

static void queue_mgr_event(u32 type, u32 id, const ws_window_t *window) {
    if (!ws_state.manager_pid) {
        return;
    }

    ws_event_t event = { 0 };
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

    bool was_empty = ring_queue_count(ws_state.mgr_queue) == 0;
    if (!_mgr_queue_push(&event)) {
        return;
    }

    if (was_empty) {
        _defer_manager_wake();
    }
}

static void queue_dirty_event(u32 id, ws_window_t *window, ws_rect_t rect) {
    if (!window || !ws_state.manager_pid || !rect.width || !rect.height) {
        return;
    }

    if (rect.x >= window->width || rect.y >= window->height) {
        return;
    }

    u32 max_width = window->width - rect.x;
    if (rect.width > max_width) {
        rect.width = max_width;
    }

    u32 max_height = window->height - rect.y;
    if (rect.height > max_height) {
        rect.height = max_height;
    }

    if (!rect.width || !rect.height) {
        return;
    }

    bool had_pending = window->mgr_dirty_pending;
    _dirty_merge(window, &rect);

    if (had_pending) {
        return;
    }

    ws_event_t event = { 0 };

    event.type = WS_EVT_WINDOW_DIRTY;
    event.id = id;
    event.owner_pid = window->owner_pid;
    event.x = (i32)window->mgr_dirty_x;
    event.y = (i32)window->mgr_dirty_y;
    event.width = window->mgr_dirty_width;
    event.height = window->mgr_dirty_height;

    bool was_empty = ring_queue_count(ws_state.mgr_queue) == 0;
    if (!_mgr_queue_push(&event)) {
        _dirty_clear(window);
        return;
    }

    if (was_empty) {
        _defer_manager_wake();
    }
}

static void _queue_dirty_write(u32 id, ws_window_t *window, size_t offset, size_t len, u32 view_width) {
    if (!window || !len || !view_width) {
        return;
    }

    size_t last_byte = len - 1;
    if (last_byte > SIZE_MAX - offset) {
        return;
    }

    size_t start_pixel = offset / sizeof(u32);
    size_t end_pixel = (offset + last_byte) / sizeof(u32);

    u32 y0 = (u32)(start_pixel / view_width);
    u32 x0 = (u32)(start_pixel % view_width);
    u32 y1 = (u32)(end_pixel / view_width);
    u32 x1 = (u32)(end_pixel % view_width);

    if (y0 == y1) {
        ws_rect_t rect = {
            .x = x0,
            .y = y0,
            .width = x1 - x0 + 1,
            .height = 1,
        };

        queue_dirty_event(id, window, rect);
        return;
    }

    ws_rect_t rect = {
        .x = 0,
        .y = y0,
        .width = view_width,
        .height = y1 - y0 + 1,
    };

    queue_dirty_event(id, window, rect);
}

static void finish_free(u32 id, bool notify_manager) {
    ws_window_t *window = _window_slot(id);
    if (!window) {
        return;
    }

    if (!window->allocated) {
        return;
    }

    _dirty_clear(window);
    _mgr_drop_dirty(id);

    ws_window_t snapshot = *window;
    _defer_window_wake(window);

    _window_slot_reinit(window);
    window->pending_ev_wake = true;

    if (notify_manager) {
        queue_mgr_event(WS_EVT_WINDOW_CLOSED, id, &snapshot);
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
        window->notify_mgr_pending = window->notify_mgr_pending || notify_manager;

        _defer_window_wake(window);
        return;
    }

    finish_free(id, notify_manager);
}

static void _window_release_io(u32 id, ws_window_t *window) {
    if (!window || !window->io_refs) {
        return;
    }

    window->io_refs--;

    if (!window->io_refs && window->pending_free) {
        bool notify_manager = window->notify_mgr_pending;
        finish_free(id, notify_manager);
    }
}

static void _clear_focus(void) {
    for (size_t i = 0; i < vec_size(ws_state.windows); i++) {
        ws_window_t *window = _window_slot((u32)i);

        if (window) {
            window->flags &= ~WS_WINDOW_FOCUSED;
        }
    }
}

static void drop_manager(pid_t manager_pid) {
    (void)manager_pid;
    ws_state.manager_pid = 0;

    ring_queue_clear(ws_state.mgr_queue);
    ws_state.pending_mgr_wake = false;
    _clear_focus();
}

static void reap_pid_locked(pid_t exited_pid) {
    if (exited_pid <= 0 || !ws_state.ready) {
        return;
    }

    if (sched_pid_alive(exited_pid)) {
        return;
    }

    bool manager_exited = ws_state.manager_pid == exited_pid;

    for (u32 i = 0; i < vec_size(ws_state.windows); i++) {
        ws_window_t *window = _window_slot(i);

        if (!window->allocated || window->owner_pid != exited_pid) {
            continue;
        }

        _free_window(i, !manager_exited);
    }

    if (manager_exited) {
        drop_manager(exited_pid);
    }
}

static void fill_cmd(ws_cmd_t *cmd, u32 id) {
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
    if (!ws_state.windows || id >= vec_size(ws_state.windows)) {
        return -EINVAL;
    }

    ws_window_t *window = _window_slot(id);
    if (!window) {
        return -ENOENT;
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

static void _copy_store(ws_window_t *window, const ws_copy_t *copy) {
    bool have_store = window && window->fb;
    bool have_stride = have_store && window->fb_store_stride;
    bool have_view = copy && copy->buf && copy->len;
    bool have_view_size = copy && copy->view_height && copy->view_stride;

    if (!have_stride || !have_view || !have_view_size) {
        return;
    }

    size_t view_stride_bytes = (size_t)copy->view_stride;
    size_t store_stride = (size_t)window->fb_store_stride;

    if (view_stride_bytes == store_stride) {
        if (copy->write) {
            memcpy(window->fb + copy->offset, copy->buf, copy->len);
        } else {
            memcpy(copy->buf, window->fb + copy->offset, copy->len);
        }
        return;
    }

    size_t done = 0;
    size_t remaining = copy->len;
    size_t offset = copy->offset;

    while (remaining > 0) {
        size_t row = offset / view_stride_bytes;
        size_t col = offset % view_stride_bytes;
        if (row >= copy->view_height) {
            break;
        }

        size_t chunk = view_stride_bytes - col;
        if (chunk > remaining) {
            chunk = remaining;
        }

        size_t store_off = row * store_stride + col;
        if (copy->write) {
            memcpy(window->fb + store_off, (const u8 *)copy->buf + done, chunk);
        } else {
            memcpy((u8 *)copy->buf + done, window->fb + store_off, chunk);
        }

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
    snprintf(slot_name, sizeof(slot_name), "%u", (unsigned int)id);

    vfs_node_t *slot = devfs_register_dir(ws_state.ws_dir, slot_name, 0755);
    if (!slot) {
        return false;
    }
    if (!_set_ws_owner(slot, "/dev/ws/<id>")) {
        return false;
    }

    void *priv = _slot_priv_encode(id);

    bool fb_registered = devfs_register_node(slot, "fb", VFS_CHARDEV, 0660, ws_state.ws_fb_if, priv);

    if (!fb_registered) {
        return false;
    }
    vfs_node_t *fb_node = vfs_lookup_from(slot, "fb");
    if (!_set_ws_owner(fb_node, "/dev/ws/<id>/fb")) {
        return false;
    }

    bool ev_registered = devfs_register_node(slot, "ev", VFS_CHARDEV, 0660, ws_state.ws_ev_if, priv);

    if (!ev_registered) {
        return false;
    }
    vfs_node_t *ev_node = vfs_lookup_from(slot, "ev");
    if (!_set_ws_owner(ev_node, "/dev/ws/<id>/ev")) {
        return false;
    }

    return true;
}

static bool _fb_layout(u32 width, u32 height, u32 *stride_out, size_t *size_out) {
    if (!width || !height || !stride_out || !size_out) {
        return false;
    }

    u64 stride = (u64)width * sizeof(u32);
    if (stride > UINT32_MAX || stride > UINT64_MAX / height) {
        return false;
    }

    u64 size = stride * height;
    if (size > WS_MAX_FB_BYTES || size > SIZE_MAX) {
        return false;
    }

    *stride_out = (u32)stride;
    *size_out = (size_t)size;
    return true;
}

static bool _fb_capacity(size_t current, size_t needed, size_t *capacity_out) {
    if (!needed || !capacity_out) {
        return false;
    }

    size_t capacity = current ? current : needed;

    while (capacity < needed) {
        size_t grown = capacity * 2;
        if (grown <= capacity) {
            capacity = needed;
            break;
        }

        capacity = grown;
    }

    if (capacity < needed || capacity > WS_MAX_FB_BYTES) {
        return false;
    }

    *capacity_out = capacity;
    return true;
}

static void _fb_copy(u8 *dst, size_t dst_stride, const ws_window_t *window) {
    size_t src_stride = window->fb_store_stride;
    size_t row_bytes = (size_t)window->fb_store_width * sizeof(u32);

    for (u32 row = 0; row < window->fb_store_height; row++) {
        const u8 *src = window->fb + (size_t)row * src_stride;
        memcpy(dst + (size_t)row * dst_stride, src, row_bytes);
    }
}

static void _fb_extend(ws_window_t *window, u32 width, u32 height, size_t stride) {
    if (!window->fb) {
        return;
    }

    u32 old_width = window->fb_store_width;
    u32 old_height = window->fb_store_height;

    if (old_width && old_height && width > old_width) {
        for (u32 row = 0; row < old_height; row++) {
            u32 *pixels = (u32 *)(window->fb + (size_t)row * stride);
            u32 edge = pixels[old_width - 1];

            for (u32 col = old_width; col < width; col++) {
                pixels[col] = edge;
            }
        }
    }

    if (height <= old_height) {
        return;
    }

    if (!old_height) {
        memset(window->fb, 0, (size_t)height * stride);
        return;
    }

    const u8 *last_row = window->fb + (size_t)(old_height - 1) * stride;

    for (u32 row = old_height; row < height; row++) {
        memcpy(window->fb + (size_t)row * stride, last_row, stride);
    }
}

static u32 _free_window_id(void) {
    u32 free_id = (u32)vec_size(ws_state.windows);

    for (u32 i = 0; i < vec_size(ws_state.windows); i++) {
        ws_window_t *slot = _window_slot(i);
        if (!slot) {
            break;
        }

        if (!slot->allocated) {
            free_id = i;
            break;
        }
    }

    return free_id;
}

static int _reserve_window_slot(u32 id, pid_t caller_pid) {
    if (!_windows_reserve((size_t)id + 1)) {
        log_warn("WS allocation failed while reserving id=%u caller=%ld", (unsigned int)id, (long)caller_pid);
        return -ENOMEM;
    }

    if (!_ensure_slot_nodes(id)) {
        log_warn(
            "WS allocation failed during slot node registration id=%u caller=%ld",
            (unsigned int)id,
            (long)caller_pid
        );
        return -ENOMEM;
    }

    return 0;
}

static int _alloc_window_buffers(ws_window_t *window, u32 id, pid_t caller_pid, size_t fb_size) {
    window->fb = calloc(1, fb_size);
    if (!window->fb) {
        log_warn(
            "WS allocation failed during fb allocation id=%u caller=%ld bytes=%zu",
            (unsigned int)id,
            (long)caller_pid,
            fb_size
        );
        return -ENOMEM;
    }

    if (!window->ev_queue) {
        window->ev_queue = ring_queue_create(sizeof(ws_input_event_t), WS_EVENT_QUEUE_CAP);
    }
    if (!window->ev_queue) {
        _window_slot_reinit(window);
        log_warn(
            "WS allocation failed during event queue reserve id=%u caller=%ld",
            (unsigned int)id,
            (long)caller_pid
        );
        return -ENOMEM;
    }

    return 0;
}

static void
_init_window_from_cmd(ws_window_t *window, u32 id, pid_t caller_pid, ws_cmd_t *cmd, u32 stride, size_t fb_size) {
    window->allocated = true;
    window->owner_pid = cmd->pid > 0 && _is_manager(caller_pid) ? cmd->pid : caller_pid;
    window->x = cmd->x;
    window->y = cmd->y;
    window->width = cmd->width;
    window->height = cmd->height;
    window->stride = stride;
    window->io_width = cmd->width;
    window->io_height = cmd->height;
    window->io_stride = stride;
    window->fb_store_width = cmd->width;
    window->fb_store_height = cmd->height;
    window->fb_store_stride = stride;
    window->fb_store_size = fb_size;
    window->z = id;
    window->flags = cmd->flags | WS_WINDOW_MAPPED;
    window->fb_size = fb_size;
    window->io_fb_size = fb_size;
    window->fb_capacity = fb_size;

    strncpy(window->title, cmd->title, sizeof(window->title) - 1);
}

static int _handle_alloc(pid_t caller_pid, ws_cmd_t *cmd) {
    if (!cmd) {
        return -EINVAL;
    }

    if (!ws_state.manager_pid) {
        return -EPIPE;
    }

    u32 stride = 0;
    size_t fb_size = 0;
    if (!_fb_layout(cmd->width, cmd->height, &stride, &fb_size)) {
        return -EINVAL;
    }

    u32 free_id = _free_window_id();
    int status = _reserve_window_slot(free_id, caller_pid);
    if (status < 0) {
        return status;
    }

    ws_window_t *window = _window_slot(free_id);
    if (!window) {
        log_warn("window slot allocation failed id=%u caller=%ld", (unsigned int)free_id, (long)caller_pid);

        return -ENOMEM;
    }

    _window_slot_reinit(window);

    status = _alloc_window_buffers(window, free_id, caller_pid, fb_size);
    if (status < 0) {
        return status;
    }

    _init_window_from_cmd(window, free_id, caller_pid, cmd, stride, fb_size);

    queue_mgr_event(WS_EVT_WINDOW_NEW, free_id, window);

    fill_cmd(cmd, free_id);
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

    fill_cmd(cmd, id);
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

    fill_cmd(cmd, cmd->id);
    return 0;
}

static int _handle_set_title(pid_t caller_pid, ws_cmd_t *cmd) {
    if (!cmd) {
        return -EINVAL;
    }

    ws_window_t *window = NULL;
    int status = _window_lookup(cmd->id, caller_pid, &window);
    if (status) {
        return status;
    }

    char title[WS_TITLE_MAX] = { 0 };
    strncpy(title, cmd->title, sizeof(title) - 1);

    if (!strncmp(window->title, title, sizeof(window->title))) {
        fill_cmd(cmd, cmd->id);
        return 0;
    }

    memset(window->title, 0, sizeof(window->title));
    strncpy(window->title, title, sizeof(window->title) - 1);

    queue_mgr_event(WS_EVT_WINDOW_TITLE, cmd->id, window);
    fill_cmd(cmd, cmd->id);
    return 0;
}

typedef struct {
    u32 view_stride;
    size_t view_size;
    u32 store_width;
    u32 store_height;
    u32 store_stride;
    size_t store_size;
    bool need_alloc;
    size_t capacity;
} ws_resize_t;

static int _plan_resize(const ws_window_t *window, const ws_cmd_t *cmd, ws_resize_t *plan) {
    if (!_fb_layout(cmd->width, cmd->height, &plan->view_stride, &plan->view_size)) {
        return -EINVAL;
    }

    plan->store_width = window->fb_store_width;
    if (cmd->width > plan->store_width || !plan->store_width) {
        plan->store_width = cmd->width;
    }

    plan->store_height = window->fb_store_height;
    if (cmd->height > plan->store_height || !plan->store_height) {
        plan->store_height = cmd->height;
    }

    if (!_fb_layout(plan->store_width, plan->store_height, &plan->store_stride, &plan->store_size)) {
        return -EINVAL;
    }

    bool stride_changed = window->fb_store_stride != plan->store_stride;
    plan->need_alloc = !window->fb || stride_changed || window->fb_capacity < plan->store_size;
    return 0;
}

static int _alloc_resize_fb(const ws_window_t *window, ws_resize_t *plan, u8 **fb_out) {
    *fb_out = NULL;

    if (!plan->need_alloc) {
        return 0;
    }

    size_t current = window->fb_capacity ? window->fb_capacity : window->fb_store_size;
    if (!_fb_capacity(current, plan->store_size, &plan->capacity)) {
        return -ENOMEM;
    }

    u8 *fb = calloc(1, plan->capacity);
    if (!fb) {
        return -ENOMEM;
    }

    if (window->fb && window->fb_store_width && window->fb_store_height && window->fb_store_stride) {
        _fb_copy(fb, plan->store_stride, window);
    }

    *fb_out = fb;
    return 0;
}

static int _queue_resize_event(ws_window_t *window, const ws_cmd_t *cmd, u32 stride, bool *was_empty) {
    ws_input_event_t event = {
        .type = INPUT_EVENT_WINDOW_RESIZE,
        .width = cmd->width,
        .height = cmd->height,
        .stride = stride,
    };

    *was_empty = ring_queue_count(window->ev_queue) == 0;
    return _window_ev_push(window, &event) ? 0 : -ENOMEM;
}

static void _commit_resize(ws_window_t *window, const ws_cmd_t *cmd, const ws_resize_t *plan, u8 *new_fb) {
    if (new_fb) {
        u8 *old_fb = window->fb;
        window->fb = new_fb;
        window->fb_capacity = plan->capacity;
        free(old_fb);
    }

    _fb_extend(window, plan->store_width, plan->store_height, plan->store_stride);

    window->fb_store_width = plan->store_width;
    window->fb_store_height = plan->store_height;
    window->fb_store_stride = plan->store_stride;
    window->fb_store_size = plan->store_size;
    window->width = cmd->width;
    window->height = cmd->height;
    window->stride = plan->view_stride;
    window->fb_size = plan->view_size;
}

static int _handle_set_size(u32 id, ws_window_t *window, ws_cmd_t *cmd) {
    if (!window || !cmd) {
        return -EINVAL;
    }

    if (cmd->width == window->width && cmd->height == window->height) {
        fill_cmd(cmd, id);
        return 0;
    }

    if (window->io_refs) {
        return -EAGAIN;
    }

    ws_resize_t plan = { 0 };
    int status = _plan_resize(window, cmd, &plan);
    if (status < 0) {
        return status;
    }

    u8 *resized_fb = NULL;
    status = _alloc_resize_fb(window, &plan, &resized_fb);
    if (status < 0) {
        return status;
    }

    bool queue_was_empty = false;
    status = _queue_resize_event(window, cmd, plan.view_stride, &queue_was_empty);
    if (status < 0) {
        free(resized_fb);
        return status;
    }

    _commit_resize(window, cmd, &plan, resized_fb);

    ws_rect_t dirty = {
        .x = 0,
        .y = 0,
        .width = window->width,
        .height = window->height,
    };

    queue_dirty_event(cmd->id, window, dirty);

    if (queue_was_empty) {
        _defer_window_wake(window);
    }

    fill_cmd(cmd, id);
    return 0;
}

static int _handle_manager_op(pid_t caller_pid, u64 request, ws_cmd_t *cmd) {
    if (!cmd) {
        return -EINVAL;
    }

    switch (request) {
    case WSIOCCLAIMMGR:
        if (ws_state.manager_pid && ws_state.manager_pid != caller_pid) {
            return -EBUSY;
        }

        ws_state.manager_pid = caller_pid;
        fill_cmd(cmd, cmd->id);

        return 0;
    case WSIOCTRANSFERMGR:
        if (!_is_manager(caller_pid)) {
            return -EPERM;
        }

        if (cmd->pid <= 0) {
            return -ESRCH;
        }

        ws_state.manager_pid = cmd->pid;
        fill_cmd(cmd, cmd->id);
        return 0;
    case WSIOCRELEASEMGR:
        if (!_is_manager(caller_pid)) {
            return -EPERM;
        }

        drop_manager(caller_pid);
        fill_cmd(cmd, cmd->id);

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
    case WSIOCSFOCUS:
        _clear_focus();
        window->flags |= WS_WINDOW_FOCUSED;
        fill_cmd(cmd, cmd->id);
        return 0;
    case WSIOCSPOS:
        window->x = cmd->x;
        window->y = cmd->y;
        fill_cmd(cmd, cmd->id);
        return 0;
    case WSIOCSSIZE:
        return _handle_set_size(cmd->id, window, cmd);
    case WSIOCSZ:
        window->z = cmd->flags;
        fill_cmd(cmd, cmd->id);
        return 0;
    case WSIOCSINPUT:
        bool ev_was_empty = ring_queue_count(window->ev_queue) == 0;
        if (!_window_ev_push(window, &cmd->input)) {
            return -ENOMEM;
        }

        if (ev_was_empty) {
            _defer_window_wake(window);
        }
        fill_cmd(cmd, cmd->id);

        return 0;
    case WSIOCCLOSE:
        _free_window(cmd->id, true);
        fill_cmd(cmd, cmd->id);

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
    ws_state.lock = (mutex_t)MUTEX_INIT;

    ws_state.windows = vec_create_sized(WS_WINDOW_INIT_CAP, sizeof(ws_window_t *));
    if (!ws_state.windows) {
        return false;
    }

    ws_state.mgr_queue = ring_queue_create(sizeof(ws_event_t), WS_MGR_QUEUE_CAP);

    if (!ws_state.mgr_queue) {
        vec_destroy(ws_state.windows);
        ws_state.windows = NULL;
        return false;
    }

    sched_waitq_init(&ws_state.mgr_wait);
    sched_waitq_set_poll(&ws_state.mgr_wait, true);

    ws_state.ready = true;

    return true;
}

static void _ws_reaper_entry(void *arg) {
    (void)arg;

    for (;;) {
        bool handled = false;
        pid_t exited_pid = 0;

        while (sched_exit_event_pop(&exited_pid)) {
            mutex_lock(&ws_state.lock);
            reap_pid_locked(exited_pid);
            mutex_unlock(&ws_state.lock);
            handled = true;
        }

        if (handled) {
            flush_wakes();
            continue;
        }

        if (!sched_is_running()) {
            arch_cpu_wait();
            continue;
        }

        u32 wait_seq = sched_exit_event_seq();
        if (sched_exit_event_pop(&exited_pid)) {
            continue;
        }

        sched_exit_wait_change(wait_seq);
    }
}

static void _ws_start_reaper(void) {
    if (ws_state.reaper_thread) {
        return;
    }

    ws_state.reaper_thread = sched_spawn_kernel("ws-reaper", _ws_reaper_entry, NULL);
    if (!ws_state.reaper_thread) {
        log_warn("failed to create reaper thread");
        return;
    }

    sched_make_runnable(ws_state.reaper_thread);
}

static sched_wait_queue_t *wsmgr_wait(vfs_node_t *node, short events, u32 flags) {
    (void)node;
    (void)flags;

    if ((events & POLLIN) == 0 || (events & ~POLLIN) != 0) {
        return NULL;
    }

    return &ws_state.mgr_wait;
}

static sched_wait_result_t _wait_mgr(u32 seq) {
    return sched_wait_on(&ws_state.mgr_wait, seq, 0, SCHED_WAIT_INTERRUPTIBLE);
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

static sched_wait_queue_t *ws_ev_wait(vfs_node_t *node, short events, u32 flags) {
    (void)flags;

    if ((events & POLLIN) == 0 || (events & ~POLLIN) != 0) {
        return NULL;
    }

    u32 id = 0;
    if (!node || !_slot_priv_decode(node->private, &id)) {
        return NULL;
    }

    mutex_lock(&ws_state.lock);
    ws_window_t *window = _window_slot(id);
    sched_wait_queue_t *queue = window ? &window->ev_wait : NULL;
    mutex_unlock(&ws_state.lock);
    return queue;
}

static bool ws_register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    if (!_ws_state_init()) {
        log_warn("window system setup failed");
        return false;
    }

    bool registered = true;

    vfs_interface_t *wsctl_if = vfs_create_interface(NULL, NULL, NULL);
    ws_state.wsctl_if = wsctl_if;

    if (!wsctl_if) {
        log_warn("failed to allocate /dev/wsctl interface");
        registered = false;
    } else {
        wsctl_if->ioctl = ws_ctl_ioctl;
        wsctl_if->poll = ws_ctl_poll;
        bool wsctl_registered = devfs_register_node(dev_dir, "wsctl", VFS_CHARDEV, 0660, wsctl_if, NULL);

        if (!wsctl_registered) {
            log_warn("failed to create /dev/wsctl");
            registered = false;
        } else {
            vfs_node_t *wsctl_node = vfs_lookup_from(dev_dir, "wsctl");
            if (!_set_ws_owner(wsctl_node, "/dev/wsctl")) {
                registered = false;
            }
        }
    }

    vfs_interface_t *wsmgr_if = vfs_create_interface(ws_mgr_read, NULL, NULL);

    ws_state.wsmgr_if = wsmgr_if;

    if (!wsmgr_if) {
        log_warn("failed to allocate /dev/wsmgr interface");
        registered = false;
    } else {
        wsmgr_if->poll = ws_mgr_poll;
        wsmgr_if->wait_queue = wsmgr_wait;
        bool wsmgr_registered = devfs_register_node(dev_dir, "wsmgr", VFS_CHARDEV, 0660, wsmgr_if, NULL);

        if (!wsmgr_registered) {
            log_warn("failed to create /dev/wsmgr");
            registered = false;
        } else {
            vfs_node_t *wsmgr_node = vfs_lookup_from(dev_dir, "wsmgr");
            if (!_set_ws_owner(wsmgr_node, "/dev/wsmgr")) {
                registered = false;
            }
        }
    }

    ws_state.ws_dir = devfs_register_dir(dev_dir, "ws", 0755);
    if (!ws_state.ws_dir) {
        log_warn("failed to create /dev/ws");
        return false;
    }
    if (!_set_ws_owner(ws_state.ws_dir, "/dev/ws")) {
        return false;
    }

    ws_state.ws_fb_if = vfs_create_interface(_dev_ws_fb_read, _dev_ws_fb_write, NULL);

    ws_state.ws_ev_if = vfs_create_interface(_dev_ws_ev_read, NULL, NULL);

    if (!ws_state.ws_fb_if || !ws_state.ws_ev_if) {
        log_warn("failed to allocate window interfaces");
        return false;
    }

    ws_state.ws_fb_if->positional_io = true;
    ws_state.ws_fb_if->poll = _dev_ws_fb_poll;
    ws_state.ws_ev_if->poll = _dev_ws_ev_poll;
    ws_state.ws_ev_if->wait_queue = ws_ev_wait;

    return registered;
}

bool ws_init(void) {
    if (!framebuffer_get_info()) {
        return true;
    }

    if (!devfs_register_device("ws", ws_register_devfs)) {
        log_warn("failed to register devfs init callback");
    }

    bool initialized = _ws_state_init();
    if (initialized) {
        _ws_start_reaper();
    }

    return initialized;
}

static ssize_t _ws_ctl_ioctl_as(pid_t caller_pid, u64 request, void *args) {
    if (!args) {
        return -EINVAL;
    }

    if (caller_pid <= 0) {
        return -EPERM;
    }

    sched_thread_t *current = sched_current();
    if (!user_write_prepare(current, args, sizeof(ws_cmd_t))) {
        return -EFAULT;
    }

    ws_cmd_t cmd = { 0 };
    if (!user_copy_from(current, &cmd, args, sizeof(cmd))) {
        return -EFAULT;
    }

    if (request == WSIOCTRANSFERMGR) {
        if (cmd.pid <= 0 || !sched_pid_alive(cmd.pid)) {
            return -ESRCH;
        }
    }

    int status = 0;

    mutex_lock(&ws_state.lock);

    switch (request) {
    case WSIOCALLOC:
        status = _handle_alloc(caller_pid, &cmd);
        break;
    case WSIOCFREE:
        status = _handle_free(caller_pid, &cmd);
        break;
    case WSIOCGINFO:
        status = _handle_query(caller_pid, &cmd);
        break;
    case WSIOCSTITLE:
        status = _handle_set_title(caller_pid, &cmd);
        break;
    case WSIOCCLAIMMGR:
    case WSIOCRELEASEMGR:
    case WSIOCTRANSFERMGR:
    case WSIOCSFOCUS:
    case WSIOCSPOS:
    case WSIOCSSIZE:
    case WSIOCSZ:
    case WSIOCSINPUT:
    case WSIOCCLOSE:
        status = _handle_manager_op(caller_pid, request, &cmd);
        break;
    default:
        status = -ENOTTY;
        break;
    }

    mutex_unlock(&ws_state.lock);
    flush_wakes();

    if (status >= 0 && !user_copy_to(current, args, &cmd, sizeof(cmd))) {
        return -EFAULT;
    }

    return status;
}

ssize_t ws_ctl_ioctl(vfs_node_t *node, u64 request, void *args) {
    (void)node;
    return _ws_ctl_ioctl_as(_current_pid(), request, args);
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

static ssize_t _ws_mgr_read_as(pid_t caller_pid, void *buf, size_t offset, size_t len, u32 flags) {
    (void)offset;

    if (!buf || len < sizeof(ws_event_t)) {
        return -EINVAL;
    }

    if (caller_pid <= 0) {
        return -EPERM;
    }

    for (;;) {
        u32 wait_seq = 0;
        mutex_lock(&ws_state.lock);

        if (!_is_manager(caller_pid)) {
            mutex_unlock(&ws_state.lock);
            return -EPERM;
        }

        if (!ws_state.mgr_queue) {
            mutex_unlock(&ws_state.lock);
            return -EIO;
        }

        if (ring_queue_count(ws_state.mgr_queue) > 0) {
            ws_event_t event;
            if (!ring_queue_pop(ws_state.mgr_queue, &event)) {
                mutex_unlock(&ws_state.lock);
                return -EIO;
            }

            if (event.type == WS_EVT_WINDOW_DIRTY) {
                ws_window_t *window = _window_slot(event.id);
                bool stale_event = !window || !window->allocated || window->owner_pid != event.owner_pid;
                bool clean_window = window && !window->mgr_dirty_pending;
                bool empty_rect = window && (!window->mgr_dirty_width || !window->mgr_dirty_height);

                if (stale_event || clean_window || empty_rect) {
                    mutex_unlock(&ws_state.lock);
                    continue;
                }

                event.x = (i32)window->mgr_dirty_x;
                event.y = (i32)window->mgr_dirty_y;
                event.width = window->mgr_dirty_width;
                event.height = window->mgr_dirty_height;
                _dirty_clear(window);
            }
            mutex_unlock(&ws_state.lock);

            memcpy(buf, &event, sizeof(event));
            return (ssize_t)sizeof(event);
        }

        if (flags & VFS_NONBLOCK) {
            mutex_unlock(&ws_state.lock);
            return -EAGAIN;
        }

        if (!sched_is_running()) {
            mutex_unlock(&ws_state.lock);
            continue;
        }

        sched_thread_t *current = sched_current();
        if (current && sched_signal_pending(current)) {
            mutex_unlock(&ws_state.lock);
            return -EINTR;
        }

        // capture sequence under ws_state.lock to avoid sleeping past an already queued event
        wait_seq = sched_wait_seq(&ws_state.mgr_wait);
        mutex_unlock(&ws_state.lock);

        sched_wait_result_t wait_result = _wait_mgr(wait_seq);

        if (wait_result == SCHED_WAIT_INTR) {
            return -EINTR;
        }
    }
}

ssize_t ws_mgr_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    return _ws_mgr_read_as(_current_pid(), buf, offset, len, flags);
}

static short _ws_mgr_poll_as(pid_t caller_pid, short events, u32 flags) {
    (void)flags;

    if (caller_pid <= 0) {
        return POLLNVAL;
    }

    mutex_lock(&ws_state.lock);

    if (!_is_manager(caller_pid)) {
        mutex_unlock(&ws_state.lock);
        return POLLNVAL;
    }

    short revents = 0;
    if ((events & POLLIN) && ring_queue_count(ws_state.mgr_queue)) {
        revents |= POLLIN;
    }
    if (events & POLLOUT) {
        revents |= POLLOUT;
    }

    mutex_unlock(&ws_state.lock);
    return revents;
}

short ws_mgr_poll(vfs_node_t *node, short events, u32 flags) {
    (void)node;
    return _ws_mgr_poll_as(_current_pid(), events, flags);
}

static ssize_t _ws_fb_read_as(const ws_io_t *io) {
    if (!io || !io->buf) {
        return -EINVAL;
    }

    (void)io->flags;

    if (io->pid <= 0) {
        return -EPERM;
    }

    mutex_lock(&ws_state.lock);

    ws_window_t *window = NULL;
    int status = _window_lookup(io->id, io->pid, &window);
    if (status) {
        mutex_unlock(&ws_state.lock);
        return status;
    }

    bool is_manager = _is_manager(io->pid);
    u32 view_height = is_manager ? window->height : window->io_height;
    u32 view_stride = is_manager ? window->stride : window->io_stride;
    size_t view_size = is_manager ? window->fb_size : window->io_fb_size;

    size_t copy_len = _copy_len(view_size, io->offset, io->len);
    if (!copy_len) {
        mutex_unlock(&ws_state.lock);
        return VFS_EOF;
    }

    window->io_refs++;
    mutex_unlock(&ws_state.lock);

    ws_copy_t copy = {
        .buf = io->buf,
        .offset = io->offset,
        .len = copy_len,
        .view_height = view_height,
        .view_stride = view_stride,
        .write = false,
    };

    mutex_lock(&window->fb_io_lock);
    _copy_store(window, &copy);
    mutex_unlock(&window->fb_io_lock);

    mutex_lock(&ws_state.lock);
    _window_release_io(io->id, window);
    mutex_unlock(&ws_state.lock);
    flush_wakes();

    return (ssize_t)copy_len;
}

ssize_t ws_fb_read(u32 id, void *buf, size_t offset, size_t len, u32 flags) {
    ws_io_t io = {
        .id = id,
        .pid = _current_pid(),
        .buf = buf,
        .offset = offset,
        .len = len,
        .flags = flags,
    };

    return _ws_fb_read_as(&io);
}

static ssize_t _ws_fb_write_as(const ws_io_t *io) {
    if (!io || !io->buf) {
        return -EINVAL;
    }

    (void)io->flags;

    if (io->pid <= 0) {
        return -EPERM;
    }

    mutex_lock(&ws_state.lock);

    ws_window_t *window = NULL;
    int status = _window_lookup(io->id, io->pid, &window);
    if (status) {
        mutex_unlock(&ws_state.lock);
        return status;
    }

    bool is_manager = _is_manager(io->pid);
    u32 view_width = is_manager ? window->width : window->io_width;
    u32 view_height = is_manager ? window->height : window->io_height;
    u32 view_stride = is_manager ? window->stride : window->io_stride;
    size_t view_size = is_manager ? window->fb_size : window->io_fb_size;

    size_t copy_len = _copy_len(view_size, io->offset, io->len);
    if (!copy_len) {
        mutex_unlock(&ws_state.lock);
        return VFS_EOF;
    }

    if (copy_len != io->len) {
        mutex_unlock(&ws_state.lock);
        return -EAGAIN;
    }

    window->io_refs++;
    mutex_unlock(&ws_state.lock);

    ws_copy_t copy = {
        .buf = io->buf,
        .offset = io->offset,
        .len = copy_len,
        .view_height = view_height,
        .view_stride = view_stride,
        .write = true,
    };

    mutex_lock(&window->fb_io_lock);
    _copy_store(window, &copy);
    mutex_unlock(&window->fb_io_lock);

    mutex_lock(&ws_state.lock);
    _queue_dirty_write(io->id, window, io->offset, copy_len, view_width);

    _window_release_io(io->id, window);
    mutex_unlock(&ws_state.lock);
    flush_wakes();

    return (ssize_t)copy_len;
}

ssize_t ws_fb_write(u32 id, const void *buf, size_t offset, size_t len, u32 flags) {
    ws_io_t io = {
        .id = id,
        .pid = _current_pid(),
        .buf = (void *)buf,
        .offset = offset,
        .len = len,
        .flags = flags,
    };

    return _ws_fb_write_as(&io);
}

void ws_notify_screen_active(void) {
    mutex_lock(&ws_state.lock);

    if (!ws_state.ready) {
        mutex_unlock(&ws_state.lock);
        return;
    }

    queue_mgr_event(WS_EVT_SCREEN_ACTIVE, 0, NULL);

    for (u32 i = 0; i < vec_size(ws_state.windows); i++) {
        ws_window_t *window = _window_slot(i);

        if (!window || !window->allocated) {
            continue;
        }

        ws_rect_t dirty = {
            .x = 0,
            .y = 0,
            .width = window->width,
            .height = window->height,
        };

        queue_dirty_event(i, window, dirty);
    }

    mutex_unlock(&ws_state.lock);
    flush_wakes();
}

static short _ws_window_poll_as(u32 id, pid_t caller_pid, short events, bool for_ev) {
    if (caller_pid <= 0) {
        return POLLNVAL;
    }

    mutex_lock(&ws_state.lock);

    ws_window_t *window = NULL;
    int status = _window_lookup(id, caller_pid, &window);

    if (status == -ENOENT) {
        mutex_unlock(&ws_state.lock);
        return POLLHUP;
    }

    if (status) {
        mutex_unlock(&ws_state.lock);
        return POLLNVAL;
    }

    short revents = 0;

    if ((events & POLLIN) && (!for_ev || ring_queue_count(window->ev_queue))) {
        revents |= POLLIN;
    }

    if (!for_ev && (events & POLLOUT)) {
        revents |= POLLOUT;
    }

    mutex_unlock(&ws_state.lock);
    return revents;
}

short ws_fb_poll(u32 id, short events, u32 flags) {
    (void)flags;
    return _ws_window_poll_as(id, _current_pid(), events, false);
}

static bool _ack_resize_event(ws_window_t *window, const ws_input_event_t *event, bool enabled) {
    if (!enabled || !window || !event || event->type != INPUT_EVENT_WINDOW_RESIZE) {
        return true;
    }

    if (!event->width || !event->height) {
        return true;
    }

    u64 auto_stride = (u64)event->width * sizeof(u32);
    if (auto_stride > UINT32_MAX) {
        return false;
    }

    u32 io_stride = event->stride ? event->stride : (u32)auto_stride;
    u64 io_fb_size = 0;

    if (io_stride <= UINT64_MAX / (u64)event->height) {
        io_fb_size = (u64)io_stride * (u64)event->height;
    }

    if (!io_stride || io_fb_size > WS_MAX_FB_BYTES) {
        return true;
    }

    window->io_width = event->width;
    window->io_height = event->height;
    window->io_stride = io_stride;
    window->io_fb_size = (size_t)io_fb_size;
    return true;
}

static ssize_t _copy_window_events(ws_window_t *window, ws_input_event_t *out, size_t max_events, bool ack_resize) {
    size_t copied = 0;

    while (copied < max_events && ring_queue_count(window->ev_queue) > 0) {
        ws_input_event_t *slot = ring_queue_at(window->ev_queue, 0);
        if (!slot) {
            return -EIO;
        }

        ws_input_event_t event = *slot;
        if (!_ack_resize_event(window, &event, ack_resize)) {
            ring_queue_drop_head(window->ev_queue);
            continue;
        }

        out[copied++] = event;
        ring_queue_drop_head(window->ev_queue);
    }

    return (ssize_t)copied;
}

static ssize_t _ws_ev_read_as(const ws_io_t *io) {
    if (!io) {
        return -EINVAL;
    }

    if (!io->buf) {
        return -EINVAL;
    }

    if (io->len < sizeof(ws_input_event_t)) {
        return -EINVAL;
    }

    size_t max_events = io->len / sizeof(ws_input_event_t);
    if (!max_events) {
        return -EINVAL;
    }

    if (io->pid <= 0) {
        return -EPERM;
    }

    for (;;) {
        ws_input_event_t *out_events = (ws_input_event_t *)io->buf;
        u32 wait_seq = 0;

        mutex_lock(&ws_state.lock);

        ws_window_t *window = NULL;
        int status = _window_lookup(io->id, io->pid, &window);

        if (status) {
            mutex_unlock(&ws_state.lock);
            return status;
        }

        ssize_t copied = _copy_window_events(window, out_events, max_events, io->pid == window->owner_pid);
        if (copied < 0) {
            mutex_unlock(&ws_state.lock);
            return copied;
        }

        if (copied) {
            mutex_unlock(&ws_state.lock);
            return (ssize_t)((size_t)copied * sizeof(ws_input_event_t));
        }

        if (io->flags & VFS_NONBLOCK) {
            mutex_unlock(&ws_state.lock);
            return -EAGAIN;
        }

        if (!sched_is_running()) {
            mutex_unlock(&ws_state.lock);
            continue;
        }

        sched_thread_t *current = sched_current();
        if (current && sched_signal_pending(current)) {
            mutex_unlock(&ws_state.lock);
            return -EINTR;
        }

        wait_seq = sched_wait_seq(&window->ev_wait);
        sched_wait_queue_t *wait_queue = &window->ev_wait;
        mutex_unlock(&ws_state.lock);

        sched_wait_result_t wait_result = sched_wait_on(wait_queue, wait_seq, 0, SCHED_WAIT_INTERRUPTIBLE);

        flush_wakes();
        if (wait_result == SCHED_WAIT_INTR) {
            return -EINTR;
        }
    }
}

ssize_t ws_ev_read(u32 id, void *buf, size_t offset, size_t len, u32 flags) {
    ws_io_t io = {
        .id = id,
        .pid = _current_pid(),
        .buf = buf,
        .offset = offset,
        .len = len,
        .flags = flags,
    };

    return _ws_ev_read_as(&io);
}

short ws_ev_poll(u32 id, short events, u32 flags) {
    (void)flags;
    return _ws_window_poll_as(id, _current_pid(), events, true);
}

bool ws_node_read(const ws_node_io_t *node_io) {
    if (!node_io || !node_io->node || !node_io->result) {
        return false;
    }

    vfs_node_t *node = node_io->node;

    if (node->interface == ws_state.wsmgr_if) {
        *node_io->result = _ws_mgr_read_as(
            node_io->pid,
            (void *)node_io->buf,
            node_io->offset,
            node_io->len,
            node_io->flags
        );
        return true;
    }

    u32 id = 0;
    if (!_slot_priv_decode(node->private, &id)) {
        return false;
    }

    if (node->interface == ws_state.ws_fb_if) {
        ws_io_t io = {
            .id = id,
            .pid = node_io->pid,
            .buf = (void *)node_io->buf,
            .offset = node_io->offset,
            .len = node_io->len,
            .flags = node_io->flags,
        };

        *node_io->result = _ws_fb_read_as(&io);
        return true;
    }

    if (node->interface == ws_state.ws_ev_if) {
        ws_io_t io = {
            .id = id,
            .pid = node_io->pid,
            .buf = (void *)node_io->buf,
            .offset = node_io->offset,
            .len = node_io->len,
            .flags = node_io->flags,
        };

        *node_io->result = _ws_ev_read_as(&io);
        return true;
    }

    return false;
}

bool ws_node_write(const ws_node_io_t *node_io) {
    u32 id = 0;
    if (!node_io || !node_io->node || !node_io->result || !_slot_priv_decode(node_io->node->private, &id)) {
        return false;
    }

    vfs_node_t *node = node_io->node;

    if (node->interface == ws_state.ws_fb_if) {
        ws_io_t io = {
            .id = id,
            .pid = node_io->pid,
            .buf = (void *)node_io->buf,
            .offset = node_io->offset,
            .len = node_io->len,
            .flags = node_io->flags,
        };

        *node_io->result = _ws_fb_write_as(&io);
        return true;
    }

    return false;
}

bool ws_node_poll(vfs_node_t *node, pid_t caller_pid, short events, u32 flags, short *result_out) {
    if (!node || !result_out) {
        return false;
    }

    if (node->interface == ws_state.wsmgr_if) {
        *result_out = _ws_mgr_poll_as(caller_pid, events, flags);
        return true;
    }

    u32 id = 0;
    if (!_slot_priv_decode(node->private, &id)) {
        return false;
    }

    if (node->interface == ws_state.ws_fb_if) {
        *result_out = _ws_window_poll_as(id, caller_pid, events, false);
        return true;
    }

    if (node->interface == ws_state.ws_ev_if) {
        *result_out = _ws_window_poll_as(id, caller_pid, events, true);
        return true;
    }

    return false;
}

bool ws_node_ioctl(vfs_node_t *node, pid_t caller_pid, u64 request, void *args, ssize_t *result_out) {
    if (!node || !result_out) {
        return false;
    }

    if (node->interface == ws_state.wsctl_if) {
        *result_out = _ws_ctl_ioctl_as(caller_pid, request, args);
        return true;
    }

    return false;
}
