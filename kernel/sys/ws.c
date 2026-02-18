#include "ws.h"

#include <arch/arch.h>
#include <errno.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/devfs.h>

#define WS_MGR_QUEUE_CAP 128
#define WS_EV_QUEUE_CAP  128
#define WS_RESP_SLOT_CAP 32
#define WS_MAX_FB_BYTES  (16U * 1024U * 1024U)

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
    u8* fb;
    size_t fb_size;
    u32 io_refs;
    bool fb_dirty;
    bool pending_free;
    bool pending_notify_manager;
    ws_input_event_t ev_queue[WS_EV_QUEUE_CAP];
    size_t ev_head;
    size_t ev_tail;
    size_t ev_count;
    sched_wait_queue_t ev_wait;
} ws_window_t;

typedef struct {
    pid_t pid;
    ws_resp_t resp;
    bool ready;
    sched_wait_queue_t wait;
} ws_resp_slot_t;

typedef struct {
    bool ready;
    pid_t manager_pid;
    ws_window_t windows[WS_MAX_WINDOWS];
    ws_event_t mgr_queue[WS_MGR_QUEUE_CAP];
    size_t mgr_head;
    size_t mgr_tail;
    size_t mgr_count;
    sched_wait_queue_t mgr_wait;
    ws_resp_slot_t resp_slots[WS_RESP_SLOT_CAP];
    bool mgr_fb_dirty;
} ws_state_t;

static ws_state_t ws_state = {0};
static volatile int ws_lock = 0;
static u32 ws_ids[WS_MAX_WINDOWS] = {0};
static bool ws_register_devfs(vfs_node_t* dev_dir);

static void _lock_acquire(void) {
    while (__sync_lock_test_and_set(&ws_lock, 1)) {
        while (ws_lock)
            arch_cpu_wait();
    }
}

static void _lock_release(void) {
    __sync_lock_release(&ws_lock);
}

static pid_t _current_pid(void) {
    sched_thread_t* current = sched_current();
    if (!current)
        return 0;

    return current->pid;
}

static bool _pid_alive(pid_t pid) {
    return pid > 0 && sched_find_thread(pid);
}

static bool _is_manager(pid_t pid) {
    return pid > 0 && ws_state.manager_pid == pid;
}

static bool _window_access(const ws_window_t* window, pid_t pid) {
    if (!window || !window->allocated || pid <= 0)
        return false;

    return window->owner_pid == pid || _is_manager(pid);
}

static ws_resp_slot_t* _find_resp_slot(pid_t pid, bool create) {
    ws_resp_slot_t* free_slot = NULL;

    for (size_t i = 0; i < WS_RESP_SLOT_CAP; i++) {
        ws_resp_slot_t* slot = &ws_state.resp_slots[i];
        if (slot->pid == pid)
            return slot;

        if (!slot->pid && !free_slot)
            free_slot = slot;
    }

    if (!create || !free_slot)
        return NULL;

    linked_list_t* wait_list = free_slot->wait.list;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->wait.list = wait_list;
    if (!free_slot->wait.list)
        sched_wait_queue_init(&free_slot->wait);
    free_slot->pid = pid;
    return free_slot;
}

static void _queue_manager_event(u32 type, u32 id, const ws_window_t* window) {
    if (!ws_state.manager_pid)
        return;

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

    if (ws_state.mgr_count == WS_MGR_QUEUE_CAP) {
        ws_state.mgr_head = (ws_state.mgr_head + 1) % WS_MGR_QUEUE_CAP;
        ws_state.mgr_count--;
    }

    ws_state.mgr_queue[ws_state.mgr_tail] = event;
    ws_state.mgr_tail = (ws_state.mgr_tail + 1) % WS_MGR_QUEUE_CAP;
    ws_state.mgr_count++;

    sched_wake_all(&ws_state.mgr_wait);
}

static void _store_response(pid_t pid, const ws_resp_t* resp) {
    if (pid <= 0 || !resp)
        return;

    ws_resp_slot_t* slot = _find_resp_slot(pid, true);
    if (!slot)
        return;

    slot->resp = *resp;
    slot->ready = true;
    sched_wake_all(&slot->wait);
}

static void _finalize_window_free(u32 id, bool notify_manager) {
    if (id >= WS_MAX_WINDOWS)
        return;

    ws_window_t* window = &ws_state.windows[id];
    if (!window->allocated)
        return;

    ws_window_t snapshot = *window;
    sched_wake_all(&window->ev_wait);

    free(window->fb);
    linked_list_t* ev_wait_list = window->ev_wait.list;

    memset(window, 0, sizeof(*window));
    window->ev_wait.list = ev_wait_list;
    if (!window->ev_wait.list)
        sched_wait_queue_init(&window->ev_wait);

    if (notify_manager)
        _queue_manager_event(WS_EVT_WINDOW_CLOSED, id, &snapshot);
}

static void _free_window(u32 id, bool notify_manager) {
    if (id >= WS_MAX_WINDOWS)
        return;

    ws_window_t* window = &ws_state.windows[id];
    if (!window->allocated)
        return;

    if (window->io_refs) {
        window->pending_free = true;
        window->pending_notify_manager = window->pending_notify_manager || notify_manager;
        sched_wake_all(&window->ev_wait);
        return;
    }

    _finalize_window_free(id, notify_manager);
}

static void _window_acquire_io(ws_window_t* window) {
    if (!window)
        return;

    window->io_refs++;
}

static void _window_release_io(u32 id, ws_window_t* window) {
    if (!window || !window->io_refs)
        return;

    window->io_refs--;

    if (!window->io_refs && window->pending_free) {
        bool notify_manager = window->pending_notify_manager;
        _finalize_window_free(id, notify_manager);
    }
}

static void _clear_focus(void) {
    for (u32 i = 0; i < WS_MAX_WINDOWS; i++)
        ws_state.windows[i].flags &= ~WS_WINDOW_FOCUSED;
}

static void _drop_manager_and_close_windows(pid_t manager_pid) {
    pid_t owners[WS_MAX_WINDOWS];
    size_t owner_count = 0;

    ws_state.manager_pid = 0;

    for (u32 i = 0; i < WS_MAX_WINDOWS; i++) {
        ws_window_t* window = &ws_state.windows[i];
        if (!window->allocated)
            continue;

        pid_t owner_pid = window->owner_pid;
        _free_window(i, false);

        if (owner_pid <= 0 || owner_pid == manager_pid)
            continue;

        bool seen = false;
        for (size_t j = 0; j < owner_count; j++) {
            if (owners[j] == owner_pid) {
                seen = true;
                break;
            }
        }

        if (!seen)
            owners[owner_count++] = owner_pid;
    }

    ws_state.mgr_head = 0;
    ws_state.mgr_tail = 0;
    ws_state.mgr_count = 0;
    _clear_focus();

    for (size_t i = 0; i < owner_count; i++)
        sched_signal_send_pid(owners[i], SIGHUP);
}

static void _reap_dead_owners(void) {
    if (ws_state.manager_pid && !_pid_alive(ws_state.manager_pid))
        _drop_manager_and_close_windows(ws_state.manager_pid);

    for (u32 i = 0; i < WS_MAX_WINDOWS; i++) {
        ws_window_t* window = &ws_state.windows[i];
        if (!window->allocated)
            continue;

        if (_pid_alive(window->owner_pid))
            continue;

        _free_window(i, true);
    }
}

static ws_resp_t _make_resp(i32 status, u32 id) {
    ws_resp_t resp = {0};
    resp.status = status;
    resp.id = id;

    if (id < WS_MAX_WINDOWS) {
        ws_window_t* window = &ws_state.windows[id];
        if (window->allocated || !status) {
            resp.x = window->x;
            resp.y = window->y;
            resp.width = window->width;
            resp.height = window->height;
            resp.stride = window->stride;
            resp.flags = window->flags;
        }
    }

    return resp;
}

static int _window_lookup(u32 id, pid_t caller_pid, ws_window_t** out) {
    if (id >= WS_MAX_WINDOWS)
        return -EINVAL;

    ws_window_t* window = &ws_state.windows[id];
    if (!window->allocated)
        return -ENOENT;

    if (window->pending_free)
        return -ENOENT;

    if (!_window_access(window, caller_pid))
        return -EPERM;

    if (out)
        *out = window;
    return 0;
}

static size_t _copy_len(size_t size, size_t offset, size_t len) {
    if (offset >= size)
        return 0;

    size_t copy_len = size - offset;
    if (copy_len > len)
        copy_len = len;

    return copy_len;
}

static int _handle_alloc(pid_t caller_pid, const ws_req_t* req, ws_resp_t* out) {
    if (!req || !out)
        return -EINVAL;

    if (!ws_state.manager_pid || !_pid_alive(ws_state.manager_pid))
        return -EPIPE;

    if (!req->width || !req->height)
        return -EINVAL;

    u64 stride = (u64)req->width * 4ULL;
    u64 fb_size_u64 = stride * (u64)req->height;
    if (!stride || fb_size_u64 > WS_MAX_FB_BYTES)
        return -EINVAL;

    u32 free_id = WS_MAX_WINDOWS;
    for (u32 i = 0; i < WS_MAX_WINDOWS; i++) {
        if (!ws_state.windows[i].allocated) {
            free_id = i;
            break;
        }
    }

    if (free_id >= WS_MAX_WINDOWS)
        return -ENOSPC;

    ws_window_t* window = &ws_state.windows[free_id];
    linked_list_t* ev_wait_list = window->ev_wait.list;
    memset(window, 0, sizeof(*window));
    window->ev_wait.list = ev_wait_list;
    if (!window->ev_wait.list)
        sched_wait_queue_init(&window->ev_wait);

    window->fb = calloc(1, (size_t)fb_size_u64);
    if (!window->fb)
        return -ENOMEM;

    window->allocated = true;
    window->owner_pid = req->pid > 0 && _is_manager(caller_pid) ? req->pid : caller_pid;
    window->x = req->x;
    window->y = req->y;
    window->width = req->width;
    window->height = req->height;
    window->stride = (u32)stride;
    window->z = free_id;
    window->flags = req->flags | WS_WINDOW_MAPPED;
    window->fb_size = (size_t)fb_size_u64;

    strncpy(window->title, req->title, sizeof(window->title) - 1);

    _queue_manager_event(WS_EVT_WINDOW_NEW, free_id, window);

    *out = _make_resp(0, free_id);
    return 0;
}

static int _handle_free(pid_t caller_pid, const ws_req_t* req, ws_resp_t* out) {
    if (!req || !out || req->id >= WS_MAX_WINDOWS)
        return -EINVAL;

    int status = _window_lookup(req->id, caller_pid, NULL);
    if (status)
        return status;

    _free_window(req->id, true);

    *out = _make_resp(0, req->id);
    return 0;
}

static int _handle_query(pid_t caller_pid, const ws_req_t* req, ws_resp_t* out) {
    if (!req || !out || req->id >= WS_MAX_WINDOWS)
        return -EINVAL;

    int status = _window_lookup(req->id, caller_pid, NULL);
    if (status)
        return status;

    *out = _make_resp(0, req->id);
    return 0;
}

static int _handle_manager_op(pid_t caller_pid, const ws_req_t* req, ws_resp_t* out) {
    if (!req || !out)
        return -EINVAL;

    switch (req->op) {
    case WS_OP_CLAIM_MANAGER:
        if (ws_state.manager_pid && ws_state.manager_pid != caller_pid)
            return -EBUSY;
        ws_state.manager_pid = caller_pid;
        *out = _make_resp(0, req->id);
        return 0;
    case WS_OP_RELEASE_MANAGER:
        if (!_is_manager(caller_pid))
            return -EPERM;
        _drop_manager_and_close_windows(caller_pid);
        *out = _make_resp(0, req->id);
        return 0;
    default:
        break;
    }

    if (!_is_manager(caller_pid))
        return -EPERM;

    if (req->id >= WS_MAX_WINDOWS)
        return -EINVAL;

    ws_window_t* window = &ws_state.windows[req->id];
    if (!window->allocated)
        return -ENOENT;

    switch (req->op) {
    case WS_OP_SET_FOCUS:
        _clear_focus();
        window->flags |= WS_WINDOW_FOCUSED;
        *out = _make_resp(0, req->id);
        return 0;
    case WS_OP_SET_POS:
        window->x = req->x;
        window->y = req->y;
        *out = _make_resp(0, req->id);
        return 0;
    case WS_OP_SET_Z:
        window->z = req->flags;
        *out = _make_resp(0, req->id);
        return 0;
    case WS_OP_SEND_INPUT:
        if (window->ev_count == WS_EV_QUEUE_CAP) {
            window->ev_head = (window->ev_head + 1) % WS_EV_QUEUE_CAP;
            window->ev_count--;
        }

        window->ev_queue[window->ev_tail] = req->input;
        window->ev_tail = (window->ev_tail + 1) % WS_EV_QUEUE_CAP;
        window->ev_count++;
        sched_wake_all(&window->ev_wait);
        *out = _make_resp(0, req->id);
        return 0;
    case WS_OP_CLOSE:
        pid_t owner_pid = window->owner_pid;
        _free_window(req->id, true);
        if (owner_pid > 0 && owner_pid != caller_pid)
            sched_signal_send_pid(owner_pid, SIGHUP);
        *out = _make_resp(0, req->id);
        return 0;
    default:
        return -EINVAL;
    }
}

bool ws_init(void) {
    if (!devfs_register_device("ws", ws_register_devfs))
        log_warn("ws: failed to register devfs init callback");

    if (ws_state.ready)
        return true;

    memset(&ws_state, 0, sizeof(ws_state));
    sched_wait_queue_init(&ws_state.mgr_wait);

    for (size_t i = 0; i < WS_MAX_WINDOWS; i++)
        sched_wait_queue_init(&ws_state.windows[i].ev_wait);

    for (size_t i = 0; i < WS_RESP_SLOT_CAP; i++)
        sched_wait_queue_init(&ws_state.resp_slots[i].wait);

    ws_state.ready = true;

    return true;
}

static ssize_t _dev_wsctl_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    return ws_ctl_read(node, buf, offset, len, flags);
}

static ssize_t _dev_wsctl_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    return ws_ctl_write(node, buf, offset, len, flags);
}

static short _dev_wsctl_poll(vfs_node_t* node, short events, u32 flags) {
    return ws_ctl_poll(node, events, flags);
}

static ssize_t _dev_ws_fb_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    if (!node || !node->private)
        return -EINVAL;

    return ws_fb_read(*(u32*)node->private, buf, offset, len, flags);
}

static ssize_t _dev_ws_fb_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    if (!node || !node->private)
        return -EINVAL;

    return ws_fb_write(*(u32*)node->private, buf, offset, len, flags);
}

static short _dev_ws_fb_poll(vfs_node_t* node, short events, u32 flags) {
    if (!node || !node->private)
        return POLLNVAL;

    return ws_fb_poll(*(u32*)node->private, events, flags);
}

static ssize_t _dev_ws_ev_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    if (!node || !node->private)
        return -EINVAL;

    return ws_ev_read(*(u32*)node->private, buf, offset, len, flags);
}

static short _dev_ws_ev_poll(vfs_node_t* node, short events, u32 flags) {
    if (!node || !node->private)
        return POLLNVAL;

    return ws_ev_poll(*(u32*)node->private, events, flags);
}

static bool ws_register_devfs(vfs_node_t* dev_dir) {
    if (!dev_dir)
        return false;

    if (!ws_init()) {
        log_warn("ws: init failed");
        return false;
    }

    bool ok = true;

    vfs_interface_t* wsctl_if = vfs_create_interface(_dev_wsctl_read, _dev_wsctl_write, NULL);
    if (!wsctl_if) {
        log_warn("ws: failed to allocate /dev/wsctl interface");
        ok = false;
    } else {
        wsctl_if->poll = _dev_wsctl_poll;
        if (!devfs_register_node(dev_dir, "wsctl", VFS_CHARDEV, 0666, wsctl_if, NULL)) {
            log_warn("ws: failed to create /dev/wsctl");
            ok = false;
        }
    }

    vfs_node_t* ws_dir = devfs_register_dir(dev_dir, "ws", 0755);
    if (!ws_dir) {
        log_warn("ws: failed to create /dev/ws");
        return false;
    }

    vfs_interface_t* ws_fb_if = vfs_create_interface(_dev_ws_fb_read, _dev_ws_fb_write, NULL);
    vfs_interface_t* ws_ev_if = vfs_create_interface(_dev_ws_ev_read, NULL, NULL);

    if (!ws_fb_if || !ws_ev_if) {
        log_warn("ws: failed to allocate per-window interfaces");
        return false;
    }

    ws_fb_if->poll = _dev_ws_fb_poll;
    ws_ev_if->poll = _dev_ws_ev_poll;

    for (u32 i = 0; i < WS_MAX_WINDOWS; i++) {
        ws_ids[i] = i;

        char slot_name[4];
        snprintf(slot_name, sizeof(slot_name), "%u", i);

        vfs_node_t* slot = devfs_register_dir(ws_dir, slot_name, 0755);
        if (!slot) {
            log_warn("ws: failed to create /dev/ws/%s", slot_name);
            ok = false;
            continue;
        }

        if (!devfs_register_node(slot, "fb", VFS_CHARDEV, 0666, ws_fb_if, &ws_ids[i])) {
            log_warn("ws: failed to create /dev/ws/%s/fb", slot_name);
            ok = false;
        }

        if (!devfs_register_node(slot, "ev", VFS_CHARDEV, 0666, ws_ev_if, &ws_ids[i])) {
            log_warn("ws: failed to create /dev/ws/%s/ev", slot_name);
            ok = false;
        }
    }

    return ok;
}

ssize_t ws_ctl_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)offset;
    (void)flags;

    if (!buf || len < sizeof(ws_req_t))
        return -EINVAL;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return -EPERM;

    ws_req_t req = {0};
    memcpy(&req, buf, sizeof(req));

    ws_resp_t resp = {0};
    int status = 0;

    _lock_acquire();
    _reap_dead_owners();

    switch (req.op) {
    case WS_OP_ALLOC:
        status = _handle_alloc(caller_pid, &req, &resp);
        break;
    case WS_OP_FREE:
        status = _handle_free(caller_pid, &req, &resp);
        break;
    case WS_OP_QUERY:
        status = _handle_query(caller_pid, &req, &resp);
        break;
    case WS_OP_CLEAR_DIRTY:
        if (_is_manager(caller_pid)) {
            ws_state.mgr_fb_dirty = false;
            for (u32 i = 0; i < WS_MAX_WINDOWS; i++)
                ws_state.windows[i].fb_dirty = false;
        }
        break;
    default:
        status = _handle_manager_op(caller_pid, &req, &resp);
        break;
    }

    if (status) {
        resp = _make_resp(status, req.id);
        resp.status = status;
    } else {
        resp.status = 0;
    }

    _store_response(caller_pid, &resp);

    _lock_release();

    return (ssize_t)sizeof(ws_req_t);
}

ssize_t ws_ctl_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)offset;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return -EPERM;

    for (;;) {
        _lock_acquire();
        _reap_dead_owners();

        ws_resp_slot_t* slot = _find_resp_slot(caller_pid, true);
        if (!slot) {
            _lock_release();
            return -ENOMEM;
        }

        if (slot->ready) {
            if (len < sizeof(ws_resp_t)) {
                _lock_release();
                return -EINVAL;
            }

            ws_resp_t resp = slot->resp;
            slot->ready = false;
            _lock_release();

            memcpy(buf, &resp, sizeof(resp));
            return (ssize_t)sizeof(resp);
        }

        if (_is_manager(caller_pid)) {
            if (len < sizeof(ws_event_t)) {
                _lock_release();
                return -EINVAL;
            }

            if (ws_state.mgr_count > 0) {
                ws_event_t event = ws_state.mgr_queue[ws_state.mgr_head];
                ws_state.mgr_head = (ws_state.mgr_head + 1) % WS_MGR_QUEUE_CAP;
                ws_state.mgr_count--;
                _lock_release();

                memcpy(buf, &event, sizeof(event));
                return (ssize_t)sizeof(event);
            }
        }

        _lock_release();

        if (flags & VFS_NONBLOCK)
            return -EAGAIN;

        if (!sched_is_running())
            continue;

        sched_thread_t* current = sched_current();
        if (current && sched_signal_has_pending(current))
            return -EINTR;

        if (_is_manager(caller_pid)) {
            sched_block(&ws_state.mgr_wait);
        } else {
            _lock_acquire();
            slot = _find_resp_slot(caller_pid, true);
            _lock_release();

            if (!slot)
                return -ENOMEM;

            sched_block(&slot->wait);
        }
    }
}

short ws_ctl_poll(vfs_node_t* node, short events, u32 flags) {
    (void)node;
    (void)flags;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return POLLNVAL;

    _lock_acquire();
    _reap_dead_owners();

    short revents = 0;

    if (events & POLLOUT)
        revents |= POLLOUT;

    if (events & POLLIN) {
        ws_resp_slot_t* slot = _find_resp_slot(caller_pid, false);
        if (slot && slot->ready)
            revents |= POLLIN;

        if (_is_manager(caller_pid)) {
            if (ws_state.mgr_count || ws_state.mgr_fb_dirty)
                revents |= POLLIN;
        } else {
            if (!slot)
                revents &= (short)~POLLIN;
        }
    }

    _lock_release();
    return revents;
}

ssize_t ws_fb_read(u32 id, void* buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!buf)
        return -EINVAL;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return -EPERM;

    _lock_acquire();

    ws_window_t* window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status) {
        _lock_release();
        return status;
    }

    size_t copy_len = _copy_len(window->fb_size, offset, len);
    if (!copy_len) {
        _lock_release();
        return VFS_EOF;
    }

    _window_acquire_io(window);
    const void* src = window->fb + offset;
    _lock_release();

    memcpy(buf, src, copy_len);

    _lock_acquire();
    _window_release_io(id, window);
    _lock_release();

    return (ssize_t)copy_len;
}

ssize_t ws_fb_write(u32 id, const void* buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!buf)
        return -EINVAL;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return -EPERM;

    _lock_acquire();

    ws_window_t* window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status) {
        _lock_release();
        return status;
    }

    size_t copy_len = _copy_len(window->fb_size, offset, len);
    if (!copy_len) {
        _lock_release();
        return VFS_EOF;
    }

    _window_acquire_io(window);
    void* dst = window->fb + offset;
    _lock_release();

    memcpy(dst, buf, copy_len);

    _lock_acquire();
    window->fb_dirty = true;
    ws_state.mgr_fb_dirty = true;
    if (ws_state.manager_pid)
        sched_wake_all(&ws_state.mgr_wait);
    _window_release_io(id, window);
    _lock_release();

    return (ssize_t)copy_len;
}

void ws_notify_screen_active(void) {
    _lock_acquire();
    ws_state.mgr_fb_dirty = true;
    if (ws_state.manager_pid)
        sched_wake_all(&ws_state.mgr_wait);
    _lock_release();
}

short ws_fb_poll(u32 id, short events, u32 flags) {
    (void)flags;

    if (id >= WS_MAX_WINDOWS)
        return POLLNVAL;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return POLLNVAL;

    _lock_acquire();

    ws_window_t* window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status == -ENOENT) {
        _lock_release();
        return POLLHUP;
    }

    if (status) {
        _lock_release();
        return POLLNVAL;
    }

    short revents = 0;
    if (events & POLLIN)
        revents |= POLLIN;
    if (events & POLLOUT)
        revents |= POLLOUT;

    _lock_release();
    return revents;
}

ssize_t ws_ev_read(u32 id, void* buf, size_t offset, size_t len, u32 flags) {
    (void)offset;

    if (!buf || id >= WS_MAX_WINDOWS)
        return -EINVAL;

    if (len < sizeof(ws_input_event_t))
        return -EINVAL;

    size_t max_events = len / sizeof(ws_input_event_t);
    if (!max_events)
        return -EINVAL;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return -EPERM;

    for (;;) {
        size_t copied = 0;

        _lock_acquire();
        _reap_dead_owners();

        ws_window_t* window = NULL;
        int status = _window_lookup(id, caller_pid, &window);
        if (status) {
            _lock_release();
            return status;
        }

        while (copied < max_events && window->ev_count > 0) {
            ((ws_input_event_t*)buf)[copied] = window->ev_queue[window->ev_head];
            window->ev_head = (window->ev_head + 1) % WS_EV_QUEUE_CAP;
            window->ev_count--;
            copied++;
        }

        if (copied) {
            _lock_release();
            return (ssize_t)(copied * sizeof(ws_input_event_t));
        }

        if (flags & VFS_NONBLOCK) {
            _lock_release();
            return -EAGAIN;
        }

        if (!sched_is_running()) {
            _lock_release();
            continue;
        }

        sched_thread_t* current = sched_current();
        if (current && sched_signal_has_pending(current)) {
            _lock_release();
            return -EINTR;
        }

        // Hold io_ref while blocking so the window (and its wait queue list)
        // cannot be finalized underneath us.
        _window_acquire_io(window);
        sched_wait_queue_t* wait_queue = &window->ev_wait;
        _lock_release();

        sched_block(wait_queue);

        _lock_acquire();
        _window_release_io(id, window);
        _lock_release();
    }
}

short ws_ev_poll(u32 id, short events, u32 flags) {
    (void)flags;

    if (id >= WS_MAX_WINDOWS)
        return POLLNVAL;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return POLLNVAL;

    _lock_acquire();
    _reap_dead_owners();

    ws_window_t* window = NULL;
    int status = _window_lookup(id, caller_pid, &window);
    if (status == -ENOENT) {
        _lock_release();
        return POLLHUP;
    }

    if (status) {
        _lock_release();
        return POLLNVAL;
    }

    short revents = 0;
    if ((events & POLLIN) && window->ev_count)
        revents |= POLLIN;

    _lock_release();
    return revents;
}
