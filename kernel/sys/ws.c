#include "ws.h"

#include <arch/arch.h>
#include <errno.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdlib.h>
#include <string.h>

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
} ws_state_t;

static ws_state_t ws_state = {0};

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

    memset(free_slot, 0, sizeof(*free_slot));
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

static void _free_window_locked(u32 id, bool notify_manager) {
    if (id >= WS_MAX_WINDOWS)
        return;

    ws_window_t* window = &ws_state.windows[id];
    if (!window->allocated)
        return;

    ws_window_t snapshot = *window;
    sched_wake_all(&window->ev_wait);

    free(window->fb);
    memset(window, 0, sizeof(*window));

    if (notify_manager)
        _queue_manager_event(WS_EVT_WINDOW_CLOSED, id, &snapshot);
}

static void _clear_focus_locked(void) {
    for (u32 i = 0; i < WS_MAX_WINDOWS; i++)
        ws_state.windows[i].flags &= ~WS_WINDOW_FOCUSED;
}

static void _reap_dead_owners_locked(void) {
    if (ws_state.manager_pid && !_pid_alive(ws_state.manager_pid))
        ws_state.manager_pid = 0;

    for (u32 i = 0; i < WS_MAX_WINDOWS; i++) {
        ws_window_t* window = &ws_state.windows[i];
        if (!window->allocated)
            continue;

        if (_pid_alive(window->owner_pid))
            continue;

        _free_window_locked(i, true);
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

static int _window_lookup_locked(u32 id, pid_t caller_pid, ws_window_t** out) {
    if (id >= WS_MAX_WINDOWS)
        return -EINVAL;

    ws_window_t* window = &ws_state.windows[id];
    if (!window->allocated)
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
    memset(window, 0, sizeof(*window));

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

    int status = _window_lookup_locked(req->id, caller_pid, NULL);
    if (status)
        return status;

    _free_window_locked(req->id, true);

    *out = _make_resp(0, req->id);
    return 0;
}

static int _handle_query(pid_t caller_pid, const ws_req_t* req, ws_resp_t* out) {
    if (!req || !out || req->id >= WS_MAX_WINDOWS)
        return -EINVAL;

    int status = _window_lookup_locked(req->id, caller_pid, NULL);
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
        ws_state.manager_pid = 0;
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
        _clear_focus_locked();
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
        _free_window_locked(req->id, true);
        if (owner_pid > 0 && owner_pid != caller_pid)
            sched_signal_send_pid(owner_pid, SIGHUP);
        *out = _make_resp(0, req->id);
        return 0;
    default:
        return -EINVAL;
    }
}

bool ws_init(void) {
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

    unsigned long irq_flags = arch_irq_save();
    _reap_dead_owners_locked();

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

    arch_irq_restore(irq_flags);

    return (ssize_t)sizeof(ws_req_t);
}

ssize_t ws_ctl_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)offset;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return -EPERM;

    for (;;) {
        unsigned long irq_flags = arch_irq_save();
        _reap_dead_owners_locked();

        if (_is_manager(caller_pid)) {
            if (len < sizeof(ws_event_t)) {
                arch_irq_restore(irq_flags);
                return -EINVAL;
            }

            if (ws_state.mgr_count > 0) {
                ws_event_t event = ws_state.mgr_queue[ws_state.mgr_head];
                ws_state.mgr_head = (ws_state.mgr_head + 1) % WS_MGR_QUEUE_CAP;
                ws_state.mgr_count--;
                arch_irq_restore(irq_flags);

                memcpy(buf, &event, sizeof(event));
                return (ssize_t)sizeof(event);
            }
        } else {
            if (len < sizeof(ws_resp_t)) {
                arch_irq_restore(irq_flags);
                return -EINVAL;
            }

            ws_resp_slot_t* slot = _find_resp_slot(caller_pid, true);
            if (!slot) {
                arch_irq_restore(irq_flags);
                return -ENOMEM;
            }

            if (slot->ready) {
                ws_resp_t resp = slot->resp;
                slot->ready = false;
                arch_irq_restore(irq_flags);

                memcpy(buf, &resp, sizeof(resp));
                return (ssize_t)sizeof(resp);
            }
        }

        arch_irq_restore(irq_flags);

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
            ws_resp_slot_t* slot = _find_resp_slot(caller_pid, true);
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

    unsigned long irq_flags = arch_irq_save();
    _reap_dead_owners_locked();

    short revents = 0;

    if (events & POLLOUT)
        revents |= POLLOUT;

    if (events & POLLIN) {
        if (_is_manager(caller_pid)) {
            if (ws_state.mgr_count)
                revents |= POLLIN;
        } else {
            ws_resp_slot_t* slot = _find_resp_slot(caller_pid, false);
            if (slot && slot->ready)
                revents |= POLLIN;
        }
    }

    arch_irq_restore(irq_flags);
    return revents;
}

ssize_t ws_fb_read(u32 id, void* buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!buf)
        return -EINVAL;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return -EPERM;

    unsigned long irq_flags = arch_irq_save();
    _reap_dead_owners_locked();

    ws_window_t* window = NULL;
    int status = _window_lookup_locked(id, caller_pid, &window);
    if (status) {
        arch_irq_restore(irq_flags);
        return status;
    }

    size_t copy_len = _copy_len(window->fb_size, offset, len);
    if (!copy_len) {
        arch_irq_restore(irq_flags);
        return VFS_EOF;
    }

    memcpy(buf, window->fb + offset, copy_len);
    arch_irq_restore(irq_flags);

    return (ssize_t)copy_len;
}

ssize_t ws_fb_write(u32 id, const void* buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!buf)
        return -EINVAL;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return -EPERM;

    unsigned long irq_flags = arch_irq_save();
    _reap_dead_owners_locked();

    ws_window_t* window = NULL;
    int status = _window_lookup_locked(id, caller_pid, &window);
    if (status) {
        arch_irq_restore(irq_flags);
        return status;
    }

    size_t copy_len = _copy_len(window->fb_size, offset, len);
    if (!copy_len) {
        arch_irq_restore(irq_flags);
        return VFS_EOF;
    }

    memcpy(window->fb + offset, buf, copy_len);
    arch_irq_restore(irq_flags);

    return (ssize_t)copy_len;
}

short ws_fb_poll(u32 id, short events, u32 flags) {
    (void)flags;

    if (id >= WS_MAX_WINDOWS)
        return POLLNVAL;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return POLLNVAL;

    unsigned long irq_flags = arch_irq_save();
    _reap_dead_owners_locked();

    ws_window_t* window = NULL;
    int status = _window_lookup_locked(id, caller_pid, &window);
    if (status == -ENOENT) {
        arch_irq_restore(irq_flags);
        return POLLHUP;
    }

    if (status) {
        arch_irq_restore(irq_flags);
        return POLLNVAL;
    }

    short revents = 0;
    if (events & POLLIN)
        revents |= POLLIN;
    if (events & POLLOUT)
        revents |= POLLOUT;

    arch_irq_restore(irq_flags);
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

        unsigned long irq_flags = arch_irq_save();
        _reap_dead_owners_locked();

        ws_window_t* window = NULL;
        int status = _window_lookup_locked(id, caller_pid, &window);
        if (status) {
            arch_irq_restore(irq_flags);
            return status;
        }

        while (copied < max_events && window->ev_count > 0) {
            ((ws_input_event_t*)buf)[copied] = window->ev_queue[window->ev_head];
            window->ev_head = (window->ev_head + 1) % WS_EV_QUEUE_CAP;
            window->ev_count--;
            copied++;
        }

        arch_irq_restore(irq_flags);

        if (copied)
            return (ssize_t)(copied * sizeof(ws_input_event_t));

        if (flags & VFS_NONBLOCK)
            return -EAGAIN;

        if (!sched_is_running())
            continue;

        sched_thread_t* current = sched_current();
        if (current && sched_signal_has_pending(current))
            return -EINTR;

        sched_block(&window->ev_wait);
    }
}

short ws_ev_poll(u32 id, short events, u32 flags) {
    (void)flags;

    if (id >= WS_MAX_WINDOWS)
        return POLLNVAL;

    pid_t caller_pid = _current_pid();
    if (caller_pid <= 0)
        return POLLNVAL;

    unsigned long irq_flags = arch_irq_save();
    _reap_dead_owners_locked();

    ws_window_t* window = NULL;
    int status = _window_lookup_locked(id, caller_pid, &window);
    if (status == -ENOENT) {
        arch_irq_restore(irq_flags);
        return POLLHUP;
    }

    if (status) {
        arch_irq_restore(irq_flags);
        return POLLNVAL;
    }

    short revents = 0;
    if ((events & POLLIN) && window->ev_count)
        revents |= POLLIN;

    arch_irq_restore(irq_flags);
    return revents;
}
