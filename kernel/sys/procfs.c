#include "procfs.h"

#include <errno.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vfs.h"

#define PROCFS_TEXT_MAX  384
#define PROCFS_WRITE_MAX 64

typedef enum {
    PROC_FIELD_STAT = 1,
    PROC_FIELD_CWD,
    PROC_FIELD_PID,
    PROC_FIELD_PPID,
    PROC_FIELD_UID,
    PROC_FIELD_GID,
    PROC_FIELD_UMASK,
    PROC_FIELD_PGID,
    PROC_FIELD_SID,
} proc_field_t;

static vfs_node_t *proc_root = NULL;


static uintptr_t _proc_key(pid_t pid, proc_field_t field) {
    return ((((uintptr_t)(u32)pid) & 0xffffffffULL) << 8) | (uintptr_t)field;
}

static proc_field_t _proc_key_field(uintptr_t key) {
    return (proc_field_t)(u8)(key & 0xff);
}

static pid_t _proc_key_pid(uintptr_t key) {
    return (pid_t)(u32)(key >> 8);
}

static char _state_char(thread_state_t state) {
    switch (state) {
    case THREAD_READY:
    case THREAD_RUNNING:
        return 'R';
    case THREAD_SLEEPING:
        return 'S';
    case THREAD_STOPPED:
        return 'T';
    case THREAD_ZOMBIE:
        return 'Z';
    default:
        return '?';
    }
}

static ssize_t
_text_read(const char *text, void *buf, size_t offset, size_t len) {
    if (!text || !buf) {
        return -EINVAL;
    }

    size_t text_len = strlen(text);
    if (offset >= text_len) {
        return VFS_EOF;
    }

    size_t copy_len = text_len - offset;
    if (copy_len > len) {
        copy_len = len;
    }

    memcpy(buf, text + offset, copy_len);
    return (ssize_t)copy_len;
}

static bool _parse_i64(const void *buf, size_t len, long long *out) {
    if (!buf || !len || !out) {
        return false;
    }

    size_t copy_len = len;
    if (copy_len >= PROCFS_WRITE_MAX) {
        copy_len = PROCFS_WRITE_MAX - 1;
    }

    char text[PROCFS_WRITE_MAX];
    memcpy(text, buf, copy_len);
    text[copy_len] = '\0';

    char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }

    char *end_trim = start + strlen(start);
    while (
        end_trim > start &&
        (
            end_trim[-1] == ' ' || 
            end_trim[-1] == '\t' ||
            end_trim[-1] == '\r' ||
            end_trim[-1] == '\n'
        )
    ) {
        end_trim--;
    }

    *end_trim = '\0';
    if (!start[0]) {
        return false;
    }

    char *end = NULL;
    long long value = strtoll(start, &end, 10);
    if (end == start || *end != '\0') {
        return false;
    }

    *out = value;
    return true;
}

static bool _resolve_pid(pid_t encoded_pid, pid_t *out_pid) {
    if (!out_pid) {
        return false;
    }

    if (!encoded_pid) {
        pid_t current = sched_getpid();
        if (current <= 0) {
            return false;
        }

        *out_pid = current;
        return true;
    }

    *out_pid = encoded_pid;
    return true;
}

static bool _proc_snapshot_from_key(
    uintptr_t key,
    sched_proc_snapshot_t *snapshot,
    pid_t *pid_out
) {
    if (!snapshot) {
        return false;
    }

    pid_t pid = 0;
    if (!_resolve_pid(_proc_key_pid(key), &pid)) {
        return false;
    }

    if (!sched_proc_snapshot(pid, snapshot)) {
        return false;
    }

    if (pid_out) {
        *pid_out = pid;
    }

    return true;
}

static bool _parse_pid_name(const char *name, pid_t *pid_out) {
    if (!name || !name[0] || !pid_out) {
        return false;
    }

    long long value = 0;

    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }

        value = value * 10 + (*p - '0');
    }

    if (value <= 0) {
        return false;
    }

    *pid_out = (pid_t)value;
    return true;
}

static bool _owner_for_pid(pid_t pid, uid_t *uid_out, gid_t *gid_out) {
    if (!uid_out || !gid_out) {
        return false;
    }

    if (!pid) {
        pid = sched_getpid();
        if (pid <= 0) {
            return false;
        }
    }

    sched_proc_snapshot_t snapshot = {0};

    if (!sched_proc_snapshot(pid, &snapshot)) {
        return false;
    }

    *uid_out = snapshot.uid;
    *gid_out = snapshot.gid;
    return true;
}

bool procfs_stat_owner(vfs_node_t *node, uid_t *uid_out, gid_t *gid_out) {
    if (!node || !uid_out || !gid_out || !proc_root) {
        return false;
    }

    if (!node->tree_entry || !node->tree_entry->parent) {
        return false;
    }

    tree_node_t *parent_tnode = node->tree_entry->parent;
    vfs_node_t *parent = parent_tnode ? parent_tnode->data : NULL;

    if (!parent || !parent->tree_entry || !parent->tree_entry->parent) {
        if (parent == proc_root && node->name && !strcmp(node->name, "self")) {
            return _owner_for_pid(0, uid_out, gid_out);
        }

        return false;
    }

    tree_node_t *grand_tnode = parent->tree_entry->parent;
    vfs_node_t *grand = grand_tnode ? grand_tnode->data : NULL;

    pid_t pid = 0;

    // /proc/self/<entry>
    if (parent == proc_root && node->name && !strcmp(node->name, "self")) {
        return _owner_for_pid(0, uid_out, gid_out);
    }

    // /proc/<pid>
    if (parent == proc_root && _parse_pid_name(node->name, &pid)) {
        return _owner_for_pid(pid, uid_out, gid_out);
    }

    // /proc/self/<entry>
    if (grand == proc_root && parent->name && !strcmp(parent->name, "self")) {
        return _owner_for_pid(0, uid_out, gid_out);
    }

    // /proc/<pid>/<entry>
    if (grand == proc_root && _parse_pid_name(parent->name, &pid)) {
        return _owner_for_pid(pid, uid_out, gid_out);
    }

    return false;
}

static ssize_t _proc_stat_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)flags;

    if (!node || !buf) {
        return -EINVAL;
    }

    sched_proc_snapshot_t snapshot = {0};
    if (!_proc_snapshot_from_key((uintptr_t)node->private, &snapshot, NULL)) {
        return -ENOENT;
    }

    char text[PROCFS_TEXT_MAX];
    snprintf(
        text,
        sizeof(text),
        "pid=%lld\n"
        "ppid=%lld\n"
        "pgid=%lld\n"
        "sid=%lld\n"
        "uid=%lld\n"
        "gid=%lld\n"
        "umask=%o\n"
        "state=%c\n"
        "tty_index=%d\n"
        "cpu_time_ms=%llu\n"
        "name=%s\n",
        (long long)snapshot.pid,
        (long long)snapshot.ppid,
        (long long)snapshot.pgid,
        (long long)snapshot.sid,
        (long long)snapshot.uid,
        (long long)snapshot.gid,
        (unsigned int)(snapshot.umask & 0777),
        _state_char(snapshot.state),
        snapshot.tty_index,
        (unsigned long long)snapshot.cpu_time_ms,
        snapshot.name
    );

    return _text_read(text, buf, offset, len);
}

static ssize_t _proc_cwd_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)flags;

    if (!node || !buf) {
        return -EINVAL;
    }

    pid_t pid = 0;
    if (!_resolve_pid(_proc_key_pid((uintptr_t)node->private), &pid)) {
        return -ENOENT;
    }

    char text[PATH_MAX];
    if (!sched_proc_cwd(pid, text, sizeof(text))) {
        return -ENOENT;
    }

    return _text_read(text, buf, offset, len);
}

static ssize_t _proc_value_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)flags;

    if (!node || !buf) {
        return -EINVAL;
    }

    uintptr_t key = (uintptr_t)node->private;
    proc_field_t field = _proc_key_field(key);

    sched_proc_snapshot_t snapshot = {0};
    if (!_proc_snapshot_from_key(key, &snapshot, NULL)) {
        return -ENOENT;
    }

    long long value = 0;

    switch (field) {
    case PROC_FIELD_PID:
        value = (long long)snapshot.pid;
        break;
    case PROC_FIELD_PPID:
        value = (long long)snapshot.ppid;
        break;
    case PROC_FIELD_UID:
        value = (long long)snapshot.uid;
        break;
    case PROC_FIELD_GID:
        value = (long long)snapshot.gid;
        break;
    case PROC_FIELD_UMASK:
        value = (long long)(snapshot.umask & 0777);
        break;
    case PROC_FIELD_PGID:
        value = (long long)snapshot.pgid;
        break;
    case PROC_FIELD_SID:
        value = (long long)snapshot.sid;
        break;
    default:
        return -EINVAL;
    }

    char text[48];
    snprintf(text, sizeof(text), "%lld\n", value);
    return _text_read(text, buf, offset, len);
}

static ssize_t _proc_value_write(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)flags;

    if (!node || !buf || !len) {
        return -EINVAL;
    }

    if (offset != 0) {
        return -EINVAL;
    }

    uintptr_t key = (uintptr_t)node->private;
    proc_field_t field = _proc_key_field(key);
    pid_t path_pid = _proc_key_pid(key);

    long long value = 0;
    if (field != PROC_FIELD_SID) {
        if (!_parse_i64(buf, len, &value)) {
            return -EINVAL;
        }
    }

    int ret = -EINVAL;

    switch (field) {
    case PROC_FIELD_UID:
        if (path_pid != 0 || value < 0) {
            return -EPERM;
        }

        ret = sched_setuid((uid_t)value);
        break;
    case PROC_FIELD_GID:
        if (path_pid != 0 || value < 0) {
            return -EPERM;
        }

        ret = sched_setgid((gid_t)value);
        break;
    case PROC_FIELD_UMASK:
        if (path_pid != 0 || value < 0) {
            return -EPERM;
        }

        ret = sched_setumask((mode_t)value & 0777);
        break;
    case PROC_FIELD_PGID:
        if (value < 0) {
            return -EINVAL;
        }

        ret = sched_setpgid(path_pid, (pid_t)value);
        break;
    case PROC_FIELD_SID:
        if (path_pid != 0) {
            return -EPERM;
        }

        ret = (int)sched_setsid();
        if (ret > 0) {
            ret = 0;
        }
        break;
    default:
        return -EINVAL;
    }

    if (ret < 0) {
        return ret;
    }

    return (ssize_t)len;
}

static bool _upsert_dir(
    vfs_node_t *parent,
    const char *name,
    mode_t mode,
    vfs_node_t **out
) {
    if (!parent || !name) {
        return false;
    }

    vfs_node_t *dir = vfs_lookup_from(parent, name);
    if (!dir) {
        dir = vfs_create_virtual(parent, (char *)name, VFS_DIR, mode);
    }

    if (!dir) {
        return false;
    }

    if (dir->interface) {
        free(dir->interface);
        dir->interface = NULL;
    }

    dir->type = VFS_DIR;
    dir->mode = mode;
    dir->private = NULL;

    if (out) {
        *out = dir;
    }

    return true;
}

static bool _upsert_file(
    vfs_node_t *parent,
    const char *name,
    mode_t mode,
    proc_field_t field,
    pid_t pid
) {
    if (!parent || !name) {
        return false;
    }

    vfs_node_t *node = vfs_lookup_from(parent, name);
    if (!node) {
        node = vfs_create_virtual(parent, (char *)name, VFS_FILE, mode);
    }

    if (!node) {
        return false;
    }

    if (!node->interface) {
        if (field == PROC_FIELD_STAT) {
            node->interface = vfs_create_interface(_proc_stat_read, NULL, NULL);
        } else if (field == PROC_FIELD_CWD) {
            node->interface = vfs_create_interface(_proc_cwd_read, NULL, NULL);
        } else {
            node->interface =
                vfs_create_interface(_proc_value_read, _proc_value_write, NULL);
        }

        if (!node->interface) {
            return false;
        }
    }

    node->type = VFS_FILE;
    node->mode = mode;
    node->private = (void *)_proc_key(pid, field);
    return true;
}

static bool _ensure_proc_entry(vfs_node_t *dir, pid_t pid, bool self) {
    if (!dir) {
        return false;
    }

    bool ok = true;
    mode_t uid_mode = self ? 0666 : 0444;
    mode_t gid_mode = self ? 0666 : 0444;
    mode_t umask_mode = self ? 0666 : 0444;
    mode_t sid_mode = self ? 0666 : 0444;

    ok &= _upsert_file(dir, "stat", 0444, PROC_FIELD_STAT, pid);
    ok &= _upsert_file(dir, "cwd", 0444, PROC_FIELD_CWD, pid);
    ok &= _upsert_file(dir, "pid", 0444, PROC_FIELD_PID, pid);
    ok &= _upsert_file(dir, "ppid", 0444, PROC_FIELD_PPID, pid);
    ok &= _upsert_file(dir, "uid", uid_mode, PROC_FIELD_UID, pid);
    ok &= _upsert_file(dir, "gid", gid_mode, PROC_FIELD_GID, pid);
    ok &= _upsert_file(dir, "umask", umask_mode, PROC_FIELD_UMASK, pid);
    ok &= _upsert_file(dir, "pgid", 0666, PROC_FIELD_PGID, pid);
    ok &= _upsert_file(dir, "sid", sid_mode, PROC_FIELD_SID, pid);

    return ok;
}

bool procfs_init(void) {
    vfs_node_t *root = vfs_lookup("/");
    if (!root) {
        log_warn("procfs missing root");
        return false;
    }

    if (VFS_IS_LINK(root->type) && root->link) {
        root = root->link;
    }

    vfs_node_t *proc = vfs_lookup("/proc");
    if (!proc) {
        proc = vfs_create_virtual(root, "proc", VFS_DIR, 0555);
    }

    if (!proc) {
        log_warn("failed to create /proc");
        return false;
    }

    if (proc->interface) {
        free(proc->interface);
        proc->interface = NULL;
    }

    proc->type = VFS_DIR;
    proc->mode = 0555;
    proc->private = NULL;
    proc_root = proc;

    vfs_node_t *self_dir = NULL;
    if (!_upsert_dir(proc_root, "self", 0555, &self_dir)) {
        log_warn("failed to create /proc/self");
        return false;
    }

    if (!_ensure_proc_entry(self_dir, 0, true)) {
        log_warn("failed to populate /proc/self");
        return false;
    }

    return true;
}

void procfs_register_pid(pid_t pid) {
    if (pid <= 0 || !proc_root) {
        return;
    }

    char pid_name[24];
    snprintf(pid_name, sizeof(pid_name), "%lld", (long long)pid);

    vfs_node_t *proc_dir = NULL;
    if (!_upsert_dir(proc_root, pid_name, 0555, &proc_dir)) {
        return;
    }

    (void)_ensure_proc_entry(proc_dir, pid, false);
}

void procfs_unregister_pid(pid_t pid) {
    if (pid <= 0 || !proc_root) {
        return;
    }

    const char *names[] = {
        "stat", "cwd", "pid", "ppid", "uid", "gid", "umask", "pgid", "sid"
    };
    char path[56];

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "/proc/%lld/%s", (long long)pid, names[i]);
        (void)vfs_unlink(path);
    }

    snprintf(path, sizeof(path), "/proc/%lld", (long long)pid);
    (void)vfs_rmdir(path);
}
