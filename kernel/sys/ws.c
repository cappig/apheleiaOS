#include "ws.h"

#include <arch/arch.h>
#include <data/ring.h>
#include <data/vector.h>
#include <errno.h>
#include <gui/input.h>
#include <inttypes.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/devfs.h>
#include <sys/framebuffer.h>
#include <sys/ioctl.h>
#include <sys/lock.h>
#include <sys/time.h>

#define WS_MGR_QUEUE_INIT_CAP 1024
#define WS_EV_QUEUE_INIT_CAP  256
#define WS_MAX_FB_BYTES       (16U * 1024U * 1024U)
#define WS_WINDOW_INIT_CAP    256
#define WS_DEV_UID            0U
#define WS_DEV_GID            46U

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
    bool pending_notify_manager;
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
} ws_state_t;

static ws_state_t ws_state = {0};
static mutex_t ws_lock = MUTEX_INIT;
static sched_thread_t *ws_reaper_thread = NULL;

static inline bool _event_is_lossy(const ws_input_event_t *event) {
    return event &&
           (event->type == INPUT_EVENT_MOUSE_MOVE ||
            event->type == INPUT_EVENT_MOUSE_WHEEL);
}

static bool _set_ws_owner(vfs_node_t *node, const char *path) {
    if (!node || !vfs_chown(node, WS_DEV_UID, WS_DEV_GID)) {
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

    free(window->fb);
    ring_queue_destroy(window->ev_queue);

    memset(window, 0, sizeof(*window));
    window->id = id;
    window->ev_wait.list = ev_wait_list;
    mutex_init(&window->fb_io_lock);
    sched_wait_queue_init(&window->ev_wait);
    sched_wait_queue_set_poll_link(&window->ev_wait, true);
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

static void _flush_deferred_wakes(void) {
    vector_t *window_wakes = vec_create(sizeof(sched_wait_queue_t *));

    for (;;) {
        bool wake_manager = false;
        bool have_single_window_wake = false;
        sched_wait_queue_t *single_window_wake = NULL;

        if (window_wakes) {
            vec_clear(window_wakes);
        }

        mutex_lock(&ws_lock);

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
                single_window_wake = &window->ev_wait;
                have_single_window_wake = true;
                break;
            }
        }

        mutex_unlock(&ws_lock);

        size_t wake_count = window_wakes ? window_wakes->size : 0;
        if (!wake_manager && !wake_count && !have_single_window_wake) {
            vec_destroy(window_wakes);
            return;
        }

        if (wake_manager) {
            // Manager queue has a single consumer; waking one avoids herd wakeups.
            sched_wake_one(&ws_state.mgr_wait);
        }

        for (size_t i = 0; i < wake_count; i++) {
            sched_wait_queue_t **wait_queue = vec_at(window_wakes, i);
            if (wait_queue && *wait_queue) {
                sched_wake_all(*wait_queue);
            }
        }

        if (have_single_window_wake && single_window_wake) {
            sched_wake_all(single_window_wake);
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
        ws_state.windows =
            vec_create_sized(WS_WINDOW_INIT_CAP, sizeof(ws_window_t *));
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
                    sched_wait_queue_destroy(&(*slot)->ev_wait);
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
                    sched_wait_queue_destroy(&(*slot)->ev_wait);
                    free(*slot);
                    *slot = NULL;
                }
            }

            sched_wait_queue_destroy(&window->ev_wait);
            free(window);

            vec_resize(ws_state.windows, old_count);
            return false;
        }
    }

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
                    _window_dirty_clear_pending(window);
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

static void _window_dirty_merge_pending(
    ws_window_t *window,
    u32 x,
    u32 y,
    u32 width,
    u32 height
) {
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

    u32 dx = window->mgr_dirty_x + window->mgr_dirty_width;
    u32 x1 = dx > (x + width) ? dx : (x + width);

    u32 dy = window->mgr_dirty_y + window->mgr_dirty_height;
    u32 y1 = dy > (y + height) ? dy : (y + height);

    window->mgr_dirty_x = x0;
    window->mgr_dirty_y = y0;
    window->mgr_dirty_width = x1 - x0;
    window->mgr_dirty_height = y1 - y0;
}

static void _mgr_queue_drop_window_dirty(u32 id) {
    ring_queue_t *q = ws_state.mgr_queue;
    if (!q) {
        return;
    }

    for (size_t i = 0; i < ring_queue_count(q); ) {
        ws_event_t *ev = ring_queue_at(q, i);
        if (ev && ev->type == WS_EVT_WINDOW_DIRTY && ev->id == id) {
            ring_queue_remove_at(q, i);
        } else {
            i++;
        }
    }
}

static bool
_window_ev_push(ws_window_t *window, const ws_input_event_t *event) {
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

    bool was_empty = ring_queue_count(ws_state.mgr_queue) == 0;
    if (!_mgr_queue_push(&event)) {
        return;
    }

    if (was_empty) {
        _defer_manager_wake();
    }
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

    bool was_empty = ring_queue_count(ws_state.mgr_queue) == 0;
    if (!_mgr_queue_push(&event)) {
        _window_dirty_clear_pending(window);
        return;
    }

    if (was_empty) {
        _defer_manager_wake();
    }
}

static void _queue_manager_dirty_write(
    u32 id,
    ws_window_t *window,
    size_t offset,
    size_t len,
    u32 view_width
) {
    if (!window || !len || !view_width) {
        return;
    }

    size_t start_pixel = offset / sizeof(u32);
    size_t end_pixel = (offset + len - 1) / sizeof(u32);

    u32 y0 = (u32)(start_pixel / view_width);
    u32 x0 = (u32)(start_pixel % view_width);
    u32 y1 = (u32)(end_pixel / view_width);
    u32 x1 = (u32)(end_pixel % view_width);

    if (y0 == y1) {
        _queue_manager_dirty_event(id, window, x0, y0, x1 - x0 + 1, 1);
        return;
    }
    _queue_manager_dirty_event(id, window, 0, y0, view_width, y1 - y0 + 1);
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
    _defer_window_wake(window);

    _window_slot_reinit(window);
    window->pending_ev_wake = true;

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
        window->pending_notify_manager =
            window->pending_notify_manager || notify_manager;

        _defer_window_wake(window);
        return;
    }

    _finalize_window_free(id, notify_manager);
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
    for (size_t i = 0; i < vec_size(ws_state.windows); i++) {
        ws_window_t *window = _window_slot((u32)i);

        if (window) {
            window->flags &= ~WS_WINDOW_FOCUSED;
        }
    }
}

static void _drop_manager_and_close_windows(pid_t manager_pid) {
    (void)manager_pid;
    ws_state.manager_pid = 0;

    ring_queue_clear(ws_state.mgr_queue);
    ws_state.pending_mgr_wake = false;
    _clear_focus();
}

static void _reap_exited_pid_locked(pid_t exited_pid) {
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
        _drop_manager_and_close_windows(exited_pid);
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

static void _copy_from_store(
    const ws_window_t *window,
    void *dst,
    size_t offset,
    size_t len,
    u32 view_height,
    u32 view_stride
) {
    if (
        !window ||
        !window->fb ||
        !dst ||
        !len ||
        !view_height ||
        !view_stride ||
        !window->fb_store_stride
    ) {
        return;
    }

    size_t view_stride_bytes = (size_t)view_stride;
    size_t store_stride = (size_t)window->fb_store_stride;

    if (view_stride_bytes == store_stride) {
        memcpy(dst, window->fb + offset, len);
        return;
    }

    u8 *out = dst;
    size_t done = 0;
    size_t remaining = len;

    while (remaining > 0) {
        size_t row = offset / view_stride_bytes;
        size_t col = offset % view_stride_bytes;
        if (row >= view_height) {
            break;
        }

        size_t chunk = view_stride_bytes - col;
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

static void _copy_to_store(
    ws_window_t *window,
    const void *src,
    size_t offset,
    size_t len,
    u32 view_height,
    u32 view_stride
) {
    if (
        !window ||
        !window->fb ||
        !src ||
        !len ||
        !view_height ||
        !view_stride ||
        !window->fb_store_stride
    ) {
        return;
    }

    size_t view_stride_bytes = (size_t)view_stride;
    size_t store_stride = (size_t)window->fb_store_stride;

    if (view_stride_bytes == store_stride) {
        memcpy(window->fb + offset, src, len);
        return;
    }

    const u8 *in = src;
    size_t done = 0;
    size_t remaining = len;

    while (remaining > 0) {
        size_t row = offset / view_stride_bytes;
        size_t col = offset % view_stride_bytes;

        if (row >= view_height) {
            break;
        }

        size_t chunk = view_stride_bytes - col;
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
    if (!_set_ws_owner(slot, "/dev/ws/<id>")) {
        return false;
    }

    void *priv = _slot_priv_encode(id);

    bool fb_registered = devfs_register_node(
        slot,
        "fb",
        VFS_CHARDEV,
        0666,
        ws_state.ws_fb_if,
        priv
    );

    if (!fb_registered) {
        return false;
    }
    vfs_node_t *fb_node = vfs_lookup_from(slot, "fb");
    if (!_set_ws_owner(fb_node, "/dev/ws/<id>/fb")) {
        return false;
    }

    bool ev_registered = devfs_register_node(
        slot,
        "ev",
        VFS_CHARDEV,
        0666,
        ws_state.ws_ev_if,
        priv
    );

    if (!ev_registered) {
        return false;
    }
    vfs_node_t *ev_node = vfs_lookup_from(slot, "ev");
    if (!_set_ws_owner(ev_node, "/dev/ws/<id>/ev")) {
        return false;
    }

    return true;
}

static int _handle_alloc(pid_t caller_pid, ws_cmd_t *cmd) {
    if (!cmd) {
        return -EINVAL;
    }

    if (!ws_state.manager_pid) {
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

    u32 free_id = (u32)vec_size(ws_state.windows);
    for (u32 i = 0; i < vec_size(ws_state.windows); i++) {
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
        log_warn(
            "WS alloc failed: reserve id=%u caller=%ld size=%ux%u",
            free_id,
            (long)caller_pid,
            cmd->width,
            cmd->height
        );
        return -ENOMEM;
    }

    if (!_ensure_slot_nodes(free_id)) {
        log_warn("WS alloc failed: slot node registration id=%u caller=%ld", free_id, (long)caller_pid);
        return -ENOMEM;
    }

    ws_window_t *window = _window_slot(free_id);
    if (!window) {
        log_warn("WS alloc failed: slot lookup id=%u caller=%ld", free_id, (long)caller_pid);
        return -ENOMEM;
    }

    _window_slot_reinit(window);

    window->fb = calloc(1, (size_t)fb_size_u64);
    if (!window->fb) {
        log_warn(
            "WS alloc failed: fb allocation id=%u caller=%ld bytes=%" PRIu64,
            free_id,
            (long)caller_pid,
            fb_size_u64
        );
        return -ENOMEM;
    }

    if (!window->ev_queue) {
        window->ev_queue = ring_queue_create(sizeof(ws_input_event_t), WS_EV_QUEUE_INIT_CAP);
    }
    if (!window->ev_queue) {
        _window_slot_reinit(window);
        log_warn(
            "WS alloc failed: ev reserve id=%u caller=%ld",
            free_id,
            (long)caller_pid
        );
        return -ENOMEM;
    }

    window->allocated = true;
    window->owner_pid =
        cmd->pid > 0 && _is_manager(caller_pid) ? cmd->pid : caller_pid;
    window->x = cmd->x;
    window->y = cmd->y;
    window->width = cmd->width;
    window->height = cmd->height;
    window->stride = (u32)stride;
    window->io_width = cmd->width;
    window->io_height = cmd->height;
    window->io_stride = (u32)stride;
    window->fb_store_width = cmd->width;
    window->fb_store_height = cmd->height;
    window->fb_store_stride = (u32)stride;
    window->fb_store_size = (size_t)fb_size_u64;
    window->z = free_id;
    window->flags = cmd->flags | WS_WINDOW_MAPPED;
    window->fb_size = (size_t)fb_size_u64;
    window->io_fb_size = (size_t)fb_size_u64;
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

static int _handle_set_title(pid_t caller_pid, ws_cmd_t *cmd) {
    if (!cmd) {
        return -EINVAL;
    }

    ws_window_t *window = NULL;
    int status = _window_lookup(cmd->id, caller_pid, &window);
    if (status) {
        return status;
    }

    char title[WS_TITLE_MAX] = {0};
    strncpy(title, cmd->title, sizeof(title) - 1);

    if (!strncmp(window->title, title, sizeof(window->title))) {
        _cmd_fill_from_window(cmd, cmd->id);
        return 0;
    }

    memset(window->title, 0, sizeof(window->title));
    strncpy(window->title, title, sizeof(window->title) - 1);

    _queue_manager_event(WS_EVT_WINDOW_TITLE, cmd->id, window);
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
    bool need_alloc =
        !window->fb || stride_change || window->fb_capacity < need_store_size;

    u8 *resized_fb = NULL;
    size_t resized_capacity = 0;

    if (need_alloc) {
        size_t new_capacity =
            window->fb_capacity ? window->fb_capacity : old_store_size;

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

    bool ev_was_empty = ring_queue_count(window->ev_queue) == 0;
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
            const u32 *edge =
                (const u32 *)(row_base + ((size_t)(old_store_width - 1) * sizeof(u32)));

            u32 fill = *edge;

            u32 *dst =
                (u32 *)(row_base + ((size_t)old_store_width * sizeof(u32)));

            for (u32 col = old_store_width; col < need_store_width; col++) {
                *dst++ = fill;
            }
        }
    }

    if (window->fb && need_store_height > old_store_height) {
        if (old_store_height > 0) {
            const u8 *src_row = 
                window->fb + ((size_t)(old_store_height - 1) * need_store_stride);

            for (u32 row = old_store_height; row < need_store_height; row++) {
                u8 *dst_row = window->fb + ((size_t)row * need_store_stride);
                memcpy(dst_row, src_row, need_store_stride);
            }
        } else {
            u8 *dst =
                window->fb + ((size_t)old_store_height * need_store_stride);

            size_t grow_bytes = 
                (size_t)(need_store_height - old_store_height) * need_store_stride;

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

    _queue_manager_dirty_event(
        cmd->id, window, 0, 0, window->width, window->height
    );

    if (ev_was_empty) {
        _defer_window_wake(window);
    }

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
    case WSIOC_TRANSFER_MANAGER:
        if (!_is_manager(caller_pid)) {
            return -EPERM;
        }

        if (cmd->pid <= 0) {
            return -ESRCH;
        }

        ws_state.manager_pid = cmd->pid;
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
        bool ev_was_empty = ring_queue_count(window->ev_queue) == 0;
        if (!_window_ev_push(window, &cmd->input)) {
            return -ENOMEM;
        }

        if (ev_was_empty) {
            _defer_window_wake(window);
        }
        _cmd_fill_from_window(cmd, cmd->id);

        return 0;
    case WSIOC_CLOSE:
        _free_window(cmd->id, true);
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

    ws_state.windows = vec_create_sized(WS_WINDOW_INIT_CAP, sizeof(ws_window_t *));
    if (!ws_state.windows) {
        return false;
    }

    ws_state.mgr_queue = ring_queue_create(sizeof(ws_event_t), WS_MGR_QUEUE_INIT_CAP);

    if (!ws_state.mgr_queue) {
        vec_destroy(ws_state.windows);
        ws_state.windows = NULL;
        return false;
    }

    sched_wait_queue_init(&ws_state.mgr_wait);
    sched_wait_queue_set_poll_link(&ws_state.mgr_wait, true);

    ws_state.ready = true;

    return true;
}

static void _ws_reaper_entry(void *arg) {
    (void)arg;

    for (;;) {
        bool handled = false;
        pid_t exited_pid = 0;

        while (sched_exit_event_pop(&exited_pid)) {
            mutex_lock(&ws_lock);
            _reap_exited_pid_locked(exited_pid);
            mutex_unlock(&ws_lock);
            handled = true;
        }

        if (handled) {
            _flush_deferred_wakes();
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

        sched_exit_event_block_if_unchanged(wait_seq);
    }
}

static void _ws_start_reaper(void) {
    if (ws_reaper_thread) {
        return;
    }

    ws_reaper_thread =
        sched_create_kernel_thread("ws-reaper", _ws_reaper_entry, NULL);
    if (!ws_reaper_thread) {
        log_warn("WS failed to create reaper thread");
        return;
    }

    sched_make_runnable(ws_reaper_thread);
}

static sched_wait_queue_t *
_dev_wsmgr_wait_queue(vfs_node_t *node, short events, u32 flags) {
    (void)node;
    (void)flags;

    if ((events & POLLIN) == 0 || (events & ~POLLIN) != 0) {
        return NULL;
    }

    return &ws_state.mgr_wait;
}

static ssize_t _dev_ws_fb_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    u32 id = 0;
    if (!node || !_slot_priv_decode(node->private, &id)) {
        return -EINVAL;
    }

    return ws_fb_read(id, buf, offset, len, flags);
}

static ssize_t _dev_ws_fb_write(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
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

static ssize_t _dev_ws_ev_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
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

static sched_wait_queue_t *
_dev_ws_ev_wait_queue(vfs_node_t *node, short events, u32 flags) {
    (void)flags;

    if ((events & POLLIN) == 0 || (events & ~POLLIN) != 0) {
        return NULL;
    }

    u32 id = 0;
    if (!node || !_slot_priv_decode(node->private, &id)) {
        return NULL;
    }

    mutex_lock(&ws_lock);
    ws_window_t *window = _window_slot(id);
    sched_wait_queue_t *queue = window ? &window->ev_wait : NULL;
    mutex_unlock(&ws_lock);
    return queue;
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
    ws_state.wsctl_if = wsctl_if;

    if (!wsctl_if) {
        log_warn("failed to allocate /dev/wsctl interface");
        ok = false;
    } else {
        wsctl_if->ioctl = ws_ctl_ioctl;
        wsctl_if->poll = ws_ctl_poll;
        bool wsctl_registered = devfs_register_node(
            dev_dir,
            "wsctl",
            VFS_CHARDEV,
            0666,
            wsctl_if,
            NULL
        );

        if (!wsctl_registered) {
            log_warn("failed to create /dev/wsctl");
            ok = false;
        } else {
            vfs_node_t *wsctl_node = vfs_lookup_from(dev_dir, "wsctl");
            if (!_set_ws_owner(wsctl_node, "/dev/wsctl")) {
                ok = false;
            }
        }
    }

    vfs_interface_t *wsmgr_if = vfs_create_interface(ws_mgr_read, NULL, NULL);

    ws_state.wsmgr_if = wsmgr_if;

    if (!wsmgr_if) {
        log_warn("failed to allocate /dev/wsmgr interface");
        ok = false;
    } else {
        wsmgr_if->poll = ws_mgr_poll;
        wsmgr_if->wait_queue = _dev_wsmgr_wait_queue;
        bool wsmgr_registered = devfs_register_node(
            dev_dir,
            "wsmgr",
            VFS_CHARDEV,
            0666,
            wsmgr_if,
            NULL
        );

        if (!wsmgr_registered) {
            log_warn("failed to create /dev/wsmgr");
            ok = false;
        } else {
            vfs_node_t *wsmgr_node = vfs_lookup_from(dev_dir, "wsmgr");
            if (!_set_ws_owner(wsmgr_node, "/dev/wsmgr")) {
                ok = false;
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

    ws_state.ws_fb_if =
        vfs_create_interface(_dev_ws_fb_read, _dev_ws_fb_write, NULL);

    ws_state.ws_ev_if = vfs_create_interface(_dev_ws_ev_read, NULL, NULL);

    if (!ws_state.ws_fb_if || !ws_state.ws_ev_if) {
        log_warn("failed to allocate window interfaces");
        return false;
    }

    ws_state.ws_fb_if->poll = _dev_ws_fb_poll;
    ws_state.ws_ev_if->poll = _dev_ws_ev_poll;
    ws_state.ws_ev_if->wait_queue = _dev_ws_ev_wait_queue;

    return ok;
}

bool ws_init(void) {
    if (!framebuffer_get_info()) {
        return true;
    }

    if (!devfs_register_device("ws", ws_register_devfs)) {
        log_warn("failed to register devfs init callback");
    }

    bool ok = _ws_state_init();
    if (ok) {
        _ws_start_reaper();
    }

    return ok;
}

static ssize_t _ws_ctl_ioctl_as(pid_t caller_pid, u64 request, void *args) {
    if (!args) {
        return -EINVAL;
    }

    if (caller_pid <= 0) {
        return -EPERM;
    }

    ws_cmd_t *cmd = args;

    if (request == WSIOC_TRANSFER_MANAGER) {
        if (cmd->pid <= 0 || !sched_pid_alive(cmd->pid)) {
            return -ESRCH;
        }
    }

    int status = 0;

    mutex_lock(&ws_lock);

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
    case WSIOC_SET_TITLE:
        status = _handle_set_title(caller_pid, cmd);
        break;
    case WSIOC_CLAIM_MANAGER:
    case WSIOC_RELEASE_MANAGER:
    case WSIOC_TRANSFER_MANAGER:
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

    mutex_unlock(&ws_lock);
    _flush_deferred_wakes();
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

static ssize_t
_ws_mgr_read_as(
    pid_t caller_pid,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)offset;

    if (!buf || len < sizeof(ws_event_t)) {
        return -EINVAL;
    }

    if (caller_pid <= 0) {
        return -EPERM;
    }

    for (;;) {
        u32 wait_seq = 0;
        mutex_lock(&ws_lock);

        if (!_is_manager(caller_pid)) {
            mutex_unlock(&ws_lock);
            return -EPERM;
        }

        if (ring_queue_count(ws_state.mgr_queue) > 0) {
            if (!ws_state.mgr_queue) {
                mutex_unlock(&ws_lock);
                return -EIO;
            }

            ws_event_t event;
            if (!ring_queue_pop(ws_state.mgr_queue, &event)) {
                mutex_unlock(&ws_lock);
                return -EIO;
            }

            if (event.type == WS_EVT_WINDOW_DIRTY) {
                ws_window_t *window = _window_slot(event.id);

                if (
                    !window ||
                    !window->allocated ||
                    window->owner_pid != event.owner_pid ||
                    !window->mgr_dirty_pending ||
                    !window->mgr_dirty_width ||
                    !window->mgr_dirty_height
                ) {
                    mutex_unlock(&ws_lock);
                    continue;
                }

                event.x = (i32)window->mgr_dirty_x;
                event.y = (i32)window->mgr_dirty_y;
                event.width = window->mgr_dirty_width;
                event.height = window->mgr_dirty_height;
                _window_dirty_clear_pending(window);
            }
            mutex_unlock(&ws_lock);

            memcpy(buf, &event, sizeof(event));
            return (ssize_t)sizeof(event);
        }

        if (flags & VFS_NONBLOCK) {
            mutex_unlock(&ws_lock);
            return -EAGAIN;
        }

        if (!sched_is_running()) {
            mutex_unlock(&ws_lock);
            continue;
        }

        sched_thread_t *current = sched_current();
        if (current && sched_signal_has_pending(current)) {
            mutex_unlock(&ws_lock);
            return -EINTR;
        }

        // Capture sequence under ws_lock to avoid sleeping past an already queued event.
        wait_seq = sched_wait_seq(&ws_state.mgr_wait);
        mutex_unlock(&ws_lock);

        sched_wait_result_t wait_result = sched_wait_on_queue(
            &ws_state.mgr_wait,
            wait_seq,
            0,
            SCHED_WAIT_INTERRUPTIBLE
        );
        if (wait_result == SCHED_WAIT_INTR) {
            return -EINTR;
        }
    }
}

ssize_t
ws_mgr_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    return _ws_mgr_read_as(_current_pid(), buf, offset, len, flags);
}

static short _ws_mgr_poll_as(pid_t caller_pid, short events, u32 flags) {
    (void)flags;

    if (caller_pid <= 0) {
        return POLLNVAL;
    }

    mutex_lock(&ws_lock);

    if (!_is_manager(caller_pid)) {
        mutex_unlock(&ws_lock);
        return POLLNVAL;
    }

    short revents = 0;
    if ((events & POLLIN) && ring_queue_count(ws_state.mgr_queue)) {
        revents |= POLLIN;
    }
    if (events & POLLOUT) {
        revents |= POLLOUT;
    }

    mutex_unlock(&ws_lock);
    return revents;
}

short ws_mgr_poll(vfs_node_t *node, short events, u32 flags) {
    (void)node;
    return _ws_mgr_poll_as(_current_pid(), events, flags);
}

static ssize_t
_ws_fb_read_as(
    u32 id,
    pid_t caller_pid,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)flags;

    if (!buf) {
        return -EINVAL;
    }

    if (caller_pid <= 0) {
        return -EPERM;
    }

    mutex_lock(&ws_lock);

    ws_window_t *window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status) {
        mutex_unlock(&ws_lock);
        return status;
    }

    bool is_manager = _is_manager(caller_pid);
    u32 view_height = is_manager ? window->height : window->io_height;
    u32 view_stride = is_manager ? window->stride : window->io_stride;
    size_t view_size = is_manager ? window->fb_size : window->io_fb_size;

    size_t copy_len = _copy_len(view_size, offset, len);
    if (!copy_len) {
        mutex_unlock(&ws_lock);
        return VFS_EOF;
    }

    window->io_refs++;
    mutex_unlock(&ws_lock);

    _copy_from_store(window, buf, offset, copy_len, view_height, view_stride);

    mutex_lock(&ws_lock);
    _window_release_io(id, window);
    mutex_unlock(&ws_lock);
    _flush_deferred_wakes();

    return (ssize_t)copy_len;
}

ssize_t ws_fb_read(u32 id, void *buf, size_t offset, size_t len, u32 flags) {
    return _ws_fb_read_as(id, _current_pid(), buf, offset, len, flags);
}

static ssize_t
_ws_fb_write_as(
    u32 id,
    pid_t caller_pid,
    const void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)flags;

    if (!buf) {
        return -EINVAL;
    }

    if (caller_pid <= 0) {
        return -EPERM;
    }

    mutex_lock(&ws_lock);

    ws_window_t *window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status) {
        mutex_unlock(&ws_lock);
        return status;
    }

    bool is_manager = _is_manager(caller_pid);
    u32 view_width = is_manager ? window->width : window->io_width;
    u32 view_height = is_manager ? window->height : window->io_height;
    u32 view_stride = is_manager ? window->stride : window->io_stride;
    size_t view_size = is_manager ? window->fb_size : window->io_fb_size;

    size_t copy_len = _copy_len(view_size, offset, len);
    if (!copy_len) {
        mutex_unlock(&ws_lock);
        return VFS_EOF;
    }

    if (copy_len != len) {
        mutex_unlock(&ws_lock);
        return -EAGAIN;
    }

    window->io_refs++;
    mutex_unlock(&ws_lock);

    _copy_to_store(window, buf, offset, copy_len, view_height, view_stride);

    mutex_lock(&ws_lock);
    _queue_manager_dirty_write(id, window, offset, copy_len, view_width);

    _window_release_io(id, window);
    mutex_unlock(&ws_lock);
    _flush_deferred_wakes();

    return (ssize_t)copy_len;
}

ssize_t
ws_fb_write(u32 id, const void *buf, size_t offset, size_t len, u32 flags) {
    return _ws_fb_write_as(id, _current_pid(), buf, offset, len, flags);
}

void ws_notify_screen_active(void) {
    mutex_lock(&ws_lock);

    if (!ws_state.ready) {
        mutex_unlock(&ws_lock);
        return;
    }

    _queue_manager_event(WS_EVT_SCREEN_ACTIVE, 0, NULL);

    for (u32 i = 0; i < vec_size(ws_state.windows); i++) {
        ws_window_t *window = _window_slot(i);
        if (!window || !window->allocated) {
            continue;
        }

        _queue_manager_dirty_event(
            i, window, 0, 0, window->width, window->height
        );
    }

    mutex_unlock(&ws_lock);
    _flush_deferred_wakes();
}

static short _ws_window_poll_as(
    u32 id,
    pid_t caller_pid,
    short events,
    bool for_ev
) {
    if (caller_pid <= 0) {
        return POLLNVAL;
    }

    mutex_lock(&ws_lock);

    ws_window_t *window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status == -ENOENT) {
        mutex_unlock(&ws_lock);
        return POLLHUP;
    }
    if (status) {
        mutex_unlock(&ws_lock);
        return POLLNVAL;
    }

    short revents = 0;
    if ((events & POLLIN) && (!for_ev || ring_queue_count(window->ev_queue))) {
        revents |= POLLIN;
    }
    if (!for_ev && (events & POLLOUT)) {
        revents |= POLLOUT;
    }

    mutex_unlock(&ws_lock);
    return revents;
}

short ws_fb_poll(u32 id, short events, u32 flags) {
    (void)flags;
    return _ws_window_poll_as(id, _current_pid(), events, false);
}

static ssize_t
_ws_ev_read_as(
    u32 id,
    pid_t caller_pid,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
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

    if (caller_pid <= 0) {
        return -EPERM;
    }

    for (;;) {
        size_t copied = 0;
        ws_input_event_t *out_events = (ws_input_event_t *)buf;
        u32 wait_seq = 0;

        mutex_lock(&ws_lock);

        ws_window_t *window = NULL;
        int status = _window_lookup(id, caller_pid, &window);
        if (status) {
            mutex_unlock(&ws_lock);
            return status;
        }

        bool apply_resize_ack = caller_pid == window->owner_pid;

        while (copied < max_events && ring_queue_count(window->ev_queue) > 0) {
            ws_input_event_t *slot = ring_queue_at(window->ev_queue, 0);
            if (!slot) {
                mutex_unlock(&ws_lock);
                return -EIO;
            }

            ws_input_event_t event = *slot;

            if (
                apply_resize_ack &&
                event.type == INPUT_EVENT_WINDOW_RESIZE &&
                event.width &&
                event.height
            ) {
                u32 io_stride = event.stride ? event.stride : event.width * sizeof(u32);
                u64 io_fb_size_u64 = (u64)io_stride * (u64)event.height;

                if (io_stride && io_fb_size_u64 <= WS_MAX_FB_BYTES) {
                    window->io_width = event.width;
                    window->io_height = event.height;
                    window->io_stride = io_stride;
                    window->io_fb_size = (size_t)io_fb_size_u64;
                }
            }

            out_events[copied] = event;
            ring_queue_drop_head(window->ev_queue);
            copied++;
        }

        if (copied) {
            mutex_unlock(&ws_lock);
            return (ssize_t)(copied * sizeof(ws_input_event_t));
        }

        if (flags & VFS_NONBLOCK) {
            mutex_unlock(&ws_lock);
            return -EAGAIN;
        }

        if (!sched_is_running()) {
            mutex_unlock(&ws_lock);
            continue;
        }

        sched_thread_t *current = sched_current();
        if (current && sched_signal_has_pending(current)) {
            mutex_unlock(&ws_lock);
            return -EINTR;
        }

        wait_seq = sched_wait_seq(&window->ev_wait);
        sched_wait_queue_t *wait_queue = &window->ev_wait;
        mutex_unlock(&ws_lock);

        sched_wait_result_t wait_result = sched_wait_on_queue(
            wait_queue,
            wait_seq,
            0,
            SCHED_WAIT_INTERRUPTIBLE
        );

        _flush_deferred_wakes();
        if (wait_result == SCHED_WAIT_INTR) {
            return -EINTR;
        }
    }
}

ssize_t ws_ev_read(u32 id, void *buf, size_t offset, size_t len, u32 flags) {
    return _ws_ev_read_as(id, _current_pid(), buf, offset, len, flags);
}

short ws_ev_poll(u32 id, short events, u32 flags) {
    (void)flags;
    return _ws_window_poll_as(id, _current_pid(), events, true);
}

bool ws_node_read(
    vfs_node_t *node,
    pid_t caller_pid,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags,
    ssize_t *result_out
) {
    if (!node || !result_out) {
        return false;
    }

    if (node->interface == ws_state.wsmgr_if) {
        *result_out = _ws_mgr_read_as(caller_pid, buf, offset, len, flags);
        return true;
    }

    u32 id = 0;
    if (!_slot_priv_decode(node->private, &id)) {
        return false;
    }

    if (node->interface == ws_state.ws_fb_if) {
        *result_out = _ws_fb_read_as(id, caller_pid, buf, offset, len, flags);
        return true;
    }

    if (node->interface == ws_state.ws_ev_if) {
        *result_out = _ws_ev_read_as(id, caller_pid, buf, offset, len, flags);
        return true;
    }

    return false;
}

bool ws_node_write(
    vfs_node_t *node,
    pid_t caller_pid,
    const void *buf,
    size_t offset,
    size_t len,
    u32 flags,
    ssize_t *result_out
) {
    u32 id = 0;
    if (!node || !result_out || !_slot_priv_decode(node->private, &id)) {
        return false;
    }

    if (node->interface == ws_state.ws_fb_if) {
        *result_out = _ws_fb_write_as(id, caller_pid, buf, offset, len, flags);
        return true;
    }

    return false;
}

bool ws_node_poll(
    vfs_node_t *node,
    pid_t caller_pid,
    short events,
    u32 flags,
    short *result_out
) {
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

bool ws_node_ioctl(
    vfs_node_t *node,
    pid_t caller_pid,
    u64 request,
    void *args,
    ssize_t *result_out
) {
    if (!node || !result_out) {
        return false;
    }

    if (node->interface == ws_state.wsctl_if) {
        *result_out = _ws_ctl_ioctl_as(caller_pid, request, args);
        return true;
    }

    return false;
}
