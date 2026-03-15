#include "procfs.h"

#include <data/list.h>
#include <errno.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>

#include "vfs.h"

#define PROCFS_TEXT_MAX  384
#define PROCFS_WRITE_MAX 256

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
    PROC_FIELD_GROUPS,
    PROC_FIELD_SIGMASK,
    PROC_FIELD_AFFINITY,
} proc_field_t;

static vfs_node_t *proc_root = NULL;
static mutex_t procfs_tree_lock = MUTEX_INIT;
static linked_list_t *procfs_dead_nodes = NULL;


static uintptr_t _proc_key(pid_t pid, proc_field_t field) {
    return ((((uintptr_t)(u32)pid) & 0xffffffffULL) << 8) | (uintptr_t)field;
}

static void _procfs_dead_add(vfs_node_t *node) {
    if (!node) {
        return;
    }

    if (!procfs_dead_nodes) {
        procfs_dead_nodes = list_create();
        if (!procfs_dead_nodes) {
            return;
        }
    }

    if (list_find(procfs_dead_nodes, node)) {
        return;
    }

    list_node_t *entry = list_create_node(node);
    if (!entry) {
        return;
    }

    if (!list_append(procfs_dead_nodes, entry)) {
        list_destroy_node(entry);
    }
}

static bool _procfs_subtree_has_refs(tree_node_t *tnode) {
    if (!tnode) {
        return false;
    }

    vfs_node_t *node = tnode->data;
    if (node && sched_fd_refs_node(node)) {
        return true;
    }

    if (!tnode->children) {
        return false;
    }

    ll_foreach(child, tnode->children) {
        tree_node_t *child_tnode = child->data;
        if (_procfs_subtree_has_refs(child_tnode)) {
            return true;
        }
    }

    return false;
}

static void _procfs_prune_tree(tree_node_t *tnode) {
    if (!tnode) {
        return;
    }

    if (tnode->children) {
        ll_foreach(child, tnode->children) {
            tree_node_t *child_tnode = child->data;
            _procfs_prune_tree(child_tnode);
        }
    }

    vfs_node_t *node = tnode->data;
    if (node) {
        vfs_destroy_node(node);
    }
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

static bool _parse_u64(const void *buf, size_t len, u64 *out) {
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
    unsigned long long value = strtoull(start, &end, 0);
    if (end == start || *end != '\0') {
        return false;
    }

    *out = (u64)value;
    return true;
}

static bool _parse_gid_list(
    const void *buf,
    size_t len,
    gid_t *groups,
    size_t max_groups,
    size_t *group_count_out
) {
    if (!buf || !groups || !group_count_out) {
        return false;
    }

    size_t copy_len = len;
    if (copy_len >= PROCFS_WRITE_MAX) {
        copy_len = PROCFS_WRITE_MAX - 1;
    }

    char text[PROCFS_WRITE_MAX];
    memcpy(text, buf, copy_len);
    text[copy_len] = '\0';

    size_t out_count = 0;
    char *save = NULL;
    char *token = strtok_r(text, " \t\r\n,", &save);
    while (token) {
        char *end = NULL;
        long value = strtol(token, &end, 10);
        if (end == token || *end != '\0' || value < 0) {
            return false;
        }

        gid_t gid = (gid_t)value;
        bool seen = false;
        for (size_t i = 0; i < out_count; i++) {
            if (groups[i] == gid) {
                seen = true;
                break;
            }
        }

        if (!seen) {
            if (out_count >= max_groups) {
                return false;
            }

            groups[out_count++] = gid;
        }

        token = strtok_r(NULL, " \t\r\n,", &save);
    }

    *group_count_out = out_count;
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

static bool _procfs_contains_node(vfs_node_t *node) {
    if (!proc_root || !node || !node->tree_entry) {
        return false;
    }

    tree_node_t *entry = node->tree_entry;
    while (entry) {
        if (entry->data == proc_root) {
            return true;
        }

        entry = entry->parent;
    }

    return false;
}

bool procfs_dir_lock_if_needed(vfs_node_t *node, unsigned long *flags_out) {
    if (!flags_out) {
        return false;
    }

    *flags_out = 0;

    if (!_procfs_contains_node(node)) {
        return false;
    }

    mutex_lock(&procfs_tree_lock);
    return true;
}

void procfs_dir_unlock_if_needed(bool locked, unsigned long flags) {
    (void)flags;
    if (locked) {
        mutex_unlock(&procfs_tree_lock);
    }
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
        "signal_pending=%u\n"
        "signal_mask=%u\n"
        "state=%c\n"
        "core_id=%d\n"
        "tty_index=%d\n"
        "cpu_time_ms=%llu\n"
        "vm_kib=%llu\n"
        "name=%s\n",
        (long long)snapshot.pid,
        (long long)snapshot.ppid,
        (long long)snapshot.pgid,
        (long long)snapshot.sid,
        (long long)snapshot.uid,
        (long long)snapshot.gid,
        (unsigned int)(snapshot.umask & 0777),
        (unsigned int)snapshot.signal_pending,
        (unsigned int)snapshot.signal_mask,
        _state_char(snapshot.state),
        snapshot.core_id,
        snapshot.tty_index,
        (unsigned long long)snapshot.cpu_time_ms,
        (unsigned long long)snapshot.vm_kib,
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
    case PROC_FIELD_SIGMASK:
        value = (long long)snapshot.signal_mask;
        break;
    default:
        return -EINVAL;
    }

    char text[48];
    snprintf(text, sizeof(text), "%lld\n", value);
    return _text_read(text, buf, offset, len);
}

static ssize_t _proc_groups_read(
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

    gid_t primary_gid = 0;
    gid_t groups[SCHED_GROUP_MAX] = {0};
    size_t group_count = 0;

    int rc = sched_getgroups_pid(
        pid,
        &primary_gid,
        groups,
        sizeof(groups) / sizeof(groups[0]),
        &group_count
    );
    if (rc < 0) {
        return rc;
    }

    char text[PROCFS_TEXT_MAX];
    size_t used = 0;
    int written = snprintf(
        text + used,
        sizeof(text) - used,
        "%llu",
        (unsigned long long)primary_gid
    );

    if (written <= 0 || (size_t)written >= sizeof(text) - used) {
        return -EIO;
    }
    used += (size_t)written;

    for (size_t i = 0; i < group_count; i++) {
        if (groups[i] == primary_gid) {
            continue;
        }

        written = snprintf(
            text + used,
            sizeof(text) - used,
            " %llu",
            (unsigned long long)groups[i]
        );

        if (written <= 0 || (size_t)written >= sizeof(text) - used) {
            break;
        }

        used += (size_t)written;
    }

    if (used + 1 >= sizeof(text)) {
        return -EIO;
    }

    text[used++] = '\n';
    text[used] = '\0';

    return _text_read(text, buf, offset, len);
}

static ssize_t _proc_groups_write(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)flags;

    if (!node || !buf || offset != 0) {
        return -EINVAL;
    }

    uintptr_t key = (uintptr_t)node->private;
    pid_t path_pid = _proc_key_pid(key);
    if (path_pid != 0) {
        return -EPERM;
    }

    gid_t groups[SCHED_GROUP_MAX] = {0};
    size_t group_count = 0;
    if (!_parse_gid_list(buf, len, groups, SCHED_GROUP_MAX, &group_count)) {
        return -EINVAL;
    }

    int rc = sched_setgroups(groups, group_count);
    if (rc < 0) {
        return rc;
    }

    return (ssize_t)len;
}

static ssize_t _proc_affinity_read(
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

    u64 mask = 0;
    int rc = sched_get_affinity(pid, &mask);
    if (rc < 0) {
        return rc;
    }

    char text[32];
    snprintf(text, sizeof(text), "0x%llx\n", (unsigned long long)mask);
    return _text_read(text, buf, offset, len);
}

static ssize_t _proc_affinity_write(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)flags;

    if (!node || !buf || !len || offset != 0) {
        return -EINVAL;
    }

    pid_t pid = 0;
    if (!_resolve_pid(_proc_key_pid((uintptr_t)node->private), &pid)) {
        return -ENOENT;
    }

    u64 mask = 0;
    if (!_parse_u64(buf, len, &mask)) {
        return -EINVAL;
    }

    int rc = sched_set_affinity(pid, mask);
    if (rc < 0) {
        return rc;
    }

    return (ssize_t)len;
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
    case PROC_FIELD_SIGMASK: {
        if (path_pid != 0) {
            return -EPERM;
        }

        if (value < 0) {
            return -EINVAL;
        }

        sched_thread_t *thread = sched_current();
        if (!thread) {
            return -EINVAL;
        }

        const u32 blockable_mask =
            ((u32)0x7fffffffU) &
            (u32) ~(1u << (SIGKILL - 1)) &
            (u32) ~(1u << (SIGSTOP - 1));

        __atomic_store_n(
            &thread->signal_mask,
            ((u32)value) & blockable_mask,
            __ATOMIC_RELEASE
        );
        ret = 0;
        break;
    }
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
        } else if (field == PROC_FIELD_GROUPS) {
            node->interface =
                vfs_create_interface(_proc_groups_read, _proc_groups_write, NULL);
        } else if (field == PROC_FIELD_AFFINITY) {
            node->interface =
                vfs_create_interface(_proc_affinity_read, _proc_affinity_write, NULL);
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
    mode_t groups_mode = self ? 0666 : 0444;
    mode_t sigmask_mode = self ? 0666 : 0444;
    mode_t affinity_mode = 0644;

    ok &= _upsert_file(dir, "stat", 0444, PROC_FIELD_STAT, pid);
    ok &= _upsert_file(dir, "cwd", 0444, PROC_FIELD_CWD, pid);
    ok &= _upsert_file(dir, "pid", 0444, PROC_FIELD_PID, pid);
    ok &= _upsert_file(dir, "ppid", 0444, PROC_FIELD_PPID, pid);
    ok &= _upsert_file(dir, "uid", uid_mode, PROC_FIELD_UID, pid);
    ok &= _upsert_file(dir, "gid", gid_mode, PROC_FIELD_GID, pid);
    ok &= _upsert_file(dir, "umask", umask_mode, PROC_FIELD_UMASK, pid);
    ok &= _upsert_file(dir, "pgid", 0666, PROC_FIELD_PGID, pid);
    ok &= _upsert_file(dir, "sid", sid_mode, PROC_FIELD_SID, pid);
    ok &= _upsert_file(dir, "groups", groups_mode, PROC_FIELD_GROUPS, pid);
    ok &= _upsert_file(dir, "sigmask", sigmask_mode, PROC_FIELD_SIGMASK, pid);
    ok &= _upsert_file(dir, "affinity", affinity_mode, PROC_FIELD_AFFINITY, pid);

    return ok;
}

bool procfs_init(void) {
    mutex_lock(&procfs_tree_lock);

    vfs_node_t *root = vfs_lookup("/");
    if (!root) {
        mutex_unlock(&procfs_tree_lock);
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
        mutex_unlock(&procfs_tree_lock);
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
        mutex_unlock(&procfs_tree_lock);
        log_warn("failed to create /proc/self");
        return false;
    }

    if (!_ensure_proc_entry(self_dir, 0, true)) {
        mutex_unlock(&procfs_tree_lock);
        log_warn("failed to populate /proc/self");
        return false;
    }

    mutex_unlock(&procfs_tree_lock);
    return true;
}

void procfs_register_pid(pid_t pid) {
    if (pid <= 0 || !proc_root) {
        return;
    }

    mutex_lock(&procfs_tree_lock);

    char pid_name[24];
    snprintf(pid_name, sizeof(pid_name), "%lld", (long long)pid);

    vfs_node_t *proc_dir = NULL;
    if (!_upsert_dir(proc_root, pid_name, 0555, &proc_dir)) {
        mutex_unlock(&procfs_tree_lock);
        return;
    }

    if (!_ensure_proc_entry(proc_dir, pid, false)) {
        log_warn("failed to populate /proc/%lld", (long long)pid);
    }

    mutex_unlock(&procfs_tree_lock);
}

void procfs_sweep_dead(void) {
    if (!procfs_dead_nodes || !procfs_dead_nodes->length) {
        return;
    }

    mutex_lock(&procfs_tree_lock);

    list_node_t *node = procfs_dead_nodes->head;
    while (node) {
        list_node_t *next = node->next;
        vfs_node_t *dir = node->data;
        tree_node_t *entry = dir ? dir->tree_entry : NULL;

        if (!dir || !entry || !_procfs_subtree_has_refs(entry)) {
            list_remove(procfs_dead_nodes, node);
            list_destroy_node(node);

            if (dir && entry) {
                _procfs_prune_tree(entry);
            }
        }

        node = next;
    }

    mutex_unlock(&procfs_tree_lock);
}

void procfs_unregister_pid(pid_t pid) {
    if (pid <= 0 || !proc_root) {
        return;
    }

    char pid_name[24];
    snprintf(pid_name, sizeof(pid_name), "%lld", (long long)pid);

    mutex_lock(&procfs_tree_lock);

    vfs_node_t *proc_dir = vfs_lookup_from(proc_root, pid_name);
    if (!proc_dir) {
        mutex_unlock(&procfs_tree_lock);
        return;
    }

    if (!vfs_detach_child(proc_root, proc_dir)) {
        mutex_unlock(&procfs_tree_lock);
        return;
    }

    _procfs_dead_add(proc_dir);
    mutex_unlock(&procfs_tree_lock);

    procfs_sweep_dead();
}
