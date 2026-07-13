#include "vfs.h"

#include <arch/arch.h>
#include <data/hashmap.h>
#include <data/list.h>
#include <errno.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/stat.h>
#include <unistd.h>

#include "panic.h"

#define VFS_MAX_SYMLINKS    16
#define VFS_PATH_CACHE_SIZE 64

typedef struct {
    char *path;
    vfs_node_t *node;
    u64 stamp;
} path_cache_entry_t;

typedef struct {
    path_cache_entry_t entries[VFS_PATH_CACHE_SIZE];
    u64 clock;
} path_cache_t;

struct vfs {
    tree_t *tree;
    path_cache_t path_cache;
    mutex_t lock;
};

static vfs_t *vfs = NULL;

// symlink resolution and path walking recurse into each other
static vfs_node_t *_walk_from_locked(vfs_node_t *from, const char *path, size_t *depth);

static bool _cacheable_path(const char *path) {
    return path && path[0] == '/';
}

// mutating operations clear the whole path cache; the table is intentionally tiny
static void _path_cache_clear(void) {
    if (!vfs) {
        return;
    }

    path_cache_t *cache = &vfs->path_cache;

    for (size_t i = 0; i < VFS_PATH_CACHE_SIZE; i++) {
        free(cache->entries[i].path);
        cache->entries[i].path = NULL;
        cache->entries[i].node = NULL;
        cache->entries[i].stamp = 0;
    }
}

static vfs_node_t *_path_cache_get(const char *path) {
    if (!vfs || !_cacheable_path(path)) {
        return NULL;
    }

    path_cache_t *cache = &vfs->path_cache;

    for (size_t i = 0; i < VFS_PATH_CACHE_SIZE; i++) {
        path_cache_entry_t *entry = &cache->entries[i];

        if (!entry->path || strcmp(entry->path, path)) {
            continue;
        }

        if (!entry->node || entry->node->removed || entry->node->busy) {
            free(entry->path);
            entry->path = NULL;
            entry->node = NULL;
            entry->stamp = 0;
            return NULL;
        }

        entry->stamp = ++cache->clock;
        return entry->node;
    }

    return NULL;
}

static void _path_cache_put(const char *path, vfs_node_t *node) {
    if (!vfs || !_cacheable_path(path) || !node || node->removed || node->busy) {
        return;
    }

    path_cache_t *cache = &vfs->path_cache;
    path_cache_entry_t *slot = NULL;

    for (size_t i = 0; i < VFS_PATH_CACHE_SIZE; i++) {
        path_cache_entry_t *entry = &cache->entries[i];

        if (entry->path && !strcmp(entry->path, path)) {
            slot = entry;
            break;
        }

        if (!entry->path) {
            slot = entry;
            break;
        }

        if (!slot || entry->stamp < slot->stamp) {
            slot = entry;
        }
    }

    if (!slot) {
        return;
    }

    if (!slot->path || strcmp(slot->path, path)) {
        char *copy = strdup(path);
        if (!copy) {
            return;
        }

        free(slot->path);
        slot->path = copy;
    }

    slot->node = node;
    slot->stamp = ++cache->clock;
}

static vfs_node_t *_link_target_locked(vfs_node_t *link, size_t *depth) {
    if (!link || !link->symlink_target || !link->symlink_target[0]) {
        return NULL;
    }

    if (link->symlink_target[0] == '/') {
        tree_node_t *root = vfs && vfs->tree ? vfs->tree->root : NULL;
        return _walk_from_locked(root ? root->data : NULL, link->symlink_target, depth);
    }

    tree_node_t *parent_entry = link->tree_entry ? link->tree_entry->parent : NULL;
    vfs_node_t *parent = parent_entry ? parent_entry->data : NULL;

    if (!parent) {
        return NULL;
    }

    return _walk_from_locked(parent, link->symlink_target, depth);
}

static vfs_node_t *_follow_link_locked(vfs_node_t *node, size_t *depth) {
    while (node && VFS_IS_LINK(node->type)) {
        if (!depth || ++(*depth) > VFS_MAX_SYMLINKS) {
            errno = ELOOP;
            return NULL;
        }

        if (node->type == VFS_MOUNT) {
            node = node->link;
            continue;
        }

        node = _link_target_locked(node, depth);
    }

    return node;
}

static const vfs_node_t *follow_mounts_const(const vfs_node_t *node) {
    size_t depth = 0;

    while (node && node->type == VFS_MOUNT) {
        if (++depth > VFS_MAX_SYMLINKS || node->link == node) {
            errno = ELOOP;
            return NULL;
        }

        node = node->link;
    }

    return node;
}

static vfs_node_t *_follow_mounts(vfs_node_t *node) {
    return (vfs_node_t *)follow_mounts_const(node);
}

static time_t _time_now(void) {
    return (time_t)(arch_realtime_ns() / 1000000000ULL);
}

static void _stamp_virtual_node(vfs_node_t *node) {
    if (!node) {
        return;
    }

    time_t now = _time_now();

    node->time.created = now;
    node->time.modified = now;
    node->time.accessed = now;
}

static bool _mount_cycle(vfs_node_t *mount, vfs_node_t *target) {
    size_t depth = 0;

    while (target && target->type == VFS_MOUNT) {
        if (target == mount || ++depth > VFS_MAX_SYMLINKS) {
            return true;
        }

        target = target->link;
    }

    return target == mount;
}

static bool _tree_has_refs(const tree_node_t *entry) {
    if (!entry) {
        return false;
    }

    const vfs_node_t *node = entry->data;
    if (node && (__atomic_load_n(&node->refs, __ATOMIC_ACQUIRE) != 0 || node->busy)) {
        return true;
    }

    if (!entry->children) {
        return false;
    }

    ll_foreach(child, entry->children) {
        if (_tree_has_refs(child->data)) {
            return true;
        }
    }

    return false;
}

static vfs_node_t *_follow_link(vfs_node_t *node) {
    if (!node || !VFS_IS_LINK(node->type)) {
        return node;
    }

    size_t depth = 0;

    mutex_lock(&vfs->lock);
    node = _follow_link_locked(node, &depth);
    mutex_unlock(&vfs->lock);

    return node;
}

static mode_t _type_mode(u32 type) {
    switch (type) {
    case VFS_DIR:
        return S_IFDIR;
    case VFS_FILE:
        return S_IFREG;
    case VFS_CHARDEV:
        return S_IFCHR;
    case VFS_BLOCKDEV:
        return S_IFBLK;
    case VFS_SYMLINK:
        return S_IFLNK;
    case VFS_MOUNT:
        return S_IFDIR;
    default:
        return 0;
    }
}

static hashmap_str_t *_child_index_map(vfs_node_t *parent, bool create) {
    if (!parent) {
        return NULL;
    }

    if (!parent->children_index && create) {
        parent->children_index = hashmap_str_create();
    }

    return parent->children_index;
}

static void _child_index_clear(vfs_node_t *node) {
    if (!node || !node->children_index) {
        return;
    }

    hashmap_str_destroy(node->children_index);
    node->children_index = NULL;
}

static bool _child_index_get(vfs_node_t *parent, const char *name, tree_node_t **tnode_out) {
    hashmap_str_t *map = _child_index_map(parent, false);
    if (!map || !name) {
        return false;
    }

    u64 encoded = 0;
    if (!hashmap_str_get(map, name, &encoded)) {
        return false;
    }

    tree_node_t *tnode = (tree_node_t *)(uintptr_t)encoded;
    if (!tnode) {
        return false;
    }

    vfs_node_t *vnode = tnode->data;
    if (!vnode || !vnode->name || strcmp(vnode->name, name)) {
        return false;
    }

    if (tnode_out) {
        *tnode_out = tnode;
    }

    return true;
}

static void _child_index_set(vfs_node_t *parent, const char *name, tree_node_t *tnode) {
    hashmap_str_t *map = _child_index_map(parent, true);
    if (!map || !name || !tnode) {
        return;
    }

    if (!hashmap_str_set(map, name, (u64)(uintptr_t)tnode)) {
        panic("vfs child index insert failed");
    }
}

static void _child_index_remove(vfs_node_t *parent, const char *name) {
    hashmap_str_t *map = _child_index_map(parent, false);
    if (!map || !name) {
        return;
    }

    if (!hashmap_str_get(map, name, NULL)) {
        return;
    }

    if (!hashmap_str_remove(map, name)) {
        panic("vfs child index remove failed");
    }
}

static void _free_node_data(vfs_node_t *node) {
    if (!node) {
        return;
    }

    _child_index_clear(node);

    fs_interface_t *node_iface = NULL;
    if (node->fs && node->fs->filesystem) {
        node_iface = node->fs->filesystem->node_interface;
    }

    if (node_iface && node_iface->destroy_node) {
        node_iface->destroy_node(node->fs, node);
    }

    vfs_clear_interface(node);

    if (node->name) {
        free(node->name);
    }

    if (node->symlink_target) {
        free(node->symlink_target);
    }

    free(node);
}

static bool _free_tree_node(tree_node_t *node) {
    if (!node) {
        return false;
    }

    _free_node_data(node->data);
    return false;
}

static vfs_node_t *_find_child_entry(vfs_node_t *parent, const char *name, tree_node_t **entry_out, bool include_busy) {
    if (!parent || !name || !parent->tree_entry) {
        return NULL;
    }

    tree_node_t *indexed = NULL;

    if (_child_index_get(parent, name, &indexed) && indexed && indexed->data) {
        vfs_node_t *vnode = indexed->data;

        if (!include_busy && vnode->busy) {
            return NULL;
        }

        if (entry_out) {
            *entry_out = indexed;
        }

        return vnode;
    }

    ll_foreach(child, parent->tree_entry->children) {
        tree_node_t *tnode = child->data;
        vfs_node_t *vnode = tnode ? tnode->data : NULL;

        if (!vnode || !vnode->name) {
            continue;
        }

        if (!strcmp(vnode->name, name)) {
            if (!include_busy && vnode->busy) {
                return NULL;
            }

            _child_index_set(parent, vnode->name, tnode);
            if (entry_out) {
                *entry_out = tnode;
            }

            return vnode;
        }
    }

    return NULL;
}

static vfs_node_t *_find_child(vfs_node_t *parent, const char *name, tree_node_t **entry_out) {
    return _find_child_entry(parent, name, entry_out, false);
}

static vfs_node_t *_find_any_child(vfs_node_t *parent, const char *name, tree_node_t **entry_out) {
    return _find_child_entry(parent, name, entry_out, true);
}

static int _remove_child(vfs_node_t *parent, vfs_node_t *child) {
    if (!parent || !child || !parent->tree_entry || !child->tree_entry) {
        return -EINVAL;
    }

    if (!tree_remove_child(parent->tree_entry, child->tree_entry)) {
        return -EIO;
    }

    _child_index_remove(parent, child->name);
    _path_cache_clear();
    child->removed = true;
    child->tree_entry->parent = NULL;

    if (__atomic_load_n(&child->refs, __ATOMIC_ACQUIRE) != 0) {
        return 0;
    }

    tree_prune_callback(child->tree_entry, _free_tree_node);
    return 0;
}

static int _detach_child(vfs_node_t *parent, vfs_node_t *child) {
    if (!parent || !child || !parent->tree_entry || !child->tree_entry) {
        return -EINVAL;
    }

    if (!tree_remove_child(parent->tree_entry, child->tree_entry)) {
        return -EIO;
    }

    _child_index_remove(parent, child->name);
    _path_cache_clear();
    child->tree_entry->parent = NULL;
    return 0;
}

static bool _is_ancestor(tree_node_t *ancestor, tree_node_t *node) {
    while (node) {
        if (node == ancestor) {
            return true;
        }

        node = node->parent;
    }

    return false;
}

static bool _dir_not_empty(vfs_node_t *node) {
    if (!node || !node->tree_entry || !node->tree_entry->children) {
        return false;
    }

    return node->tree_entry->children->length != 0;
}

static bool _same_inode(vfs_node_t *a, vfs_node_t *b) {
    if (!a || !b || !a->fs || !a->inode) {
        return false;
    }

    return a->fs == b->fs && a->inode == b->inode;
}

static int check_target(vfs_node_t *child, vfs_node_t *target) {
    if (!target) {
        return 0;
    }

    bool child_dir = child->type == VFS_DIR;
    bool target_dir = target->type == VFS_DIR;

    if (child->type == VFS_MOUNT || target->type == VFS_MOUNT) {
        return -EBUSY;
    }

    if (child_dir && !target_dir) {
        return -ENOTDIR;
    }

    if (!child_dir && target_dir) {
        return -EISDIR;
    }

    if (target_dir && _dir_not_empty(target)) {
        return -ENOTEMPTY;
    }

    if (_same_inode(child, target)) {
        return 1;
    }

    return 0;
}

typedef struct {
    vfs_node_t *old_parent;
    vfs_node_t *new_parent;
    vfs_node_t *target;
    vfs_node_t *child;
    tree_node_t *child_entry;
    char *old_base;
    char *new_base;
    char *new_name;
} rename_op_t;

static void _rename_op_clear(rename_op_t *op) {
    if (!op) {
        return;
    }

    free(op->old_base);
    free(op->new_base);
    free(op->new_name);
}

static void _rename_restore_old(rename_op_t *op, char *old_name) {
    char *failed_name = op->child->name;

    op->child_entry->parent = NULL;
    op->child->name = old_name;

    bool restored = tree_insert_child(op->old_parent->tree_entry, op->child_entry);
    assert(restored);

    _child_index_set(op->old_parent, op->child->name, op->child_entry);
    free(failed_name);
}

static int _rename_tree(rename_op_t *op) {
    if (!tree_remove_child(op->old_parent->tree_entry, op->child_entry)) {
        return -EIO;
    }

    char *old_name = op->child->name;

    op->child_entry->parent = NULL;
    op->child->name = op->new_name;
    op->new_name = NULL;

    _child_index_remove(op->old_parent, op->old_base);

    if (!tree_insert_child(op->new_parent->tree_entry, op->child_entry)) {
        _rename_restore_old(op, old_name);
        return -ENOMEM;
    }

    if (op->target) {
        int status = _remove_child(op->new_parent, op->target);
        if (status < 0) {
            bool removed = tree_remove_child(op->new_parent->tree_entry, op->child_entry);
            assert(removed);

            _rename_restore_old(op, old_name);
            return status;
        }
    }

    free(old_name);
    _child_index_set(op->new_parent, op->child->name, op->child_entry);
    _path_cache_clear();
    return 0;
}

static int _rename_fs_node(
    vfs_node_t *old_parent,
    vfs_node_t *child,
    vfs_node_t *new_parent,
    vfs_node_t *target,
    const char *new_name
) {
    bool persistent = old_parent->fs || new_parent->fs || child->fs;

    if (!persistent) {
        return 0;
    }

    bool same_fs = old_parent->fs == new_parent->fs && child->fs == old_parent->fs;
    same_fs = same_fs && (!target || target->fs == child->fs);

    if (!same_fs) {
        return -EXDEV;
    }

    if (!old_parent->interface || !old_parent->interface->rename) {
        return -ENOTSUP;
    }

    ssize_t status = old_parent->interface->rename(old_parent, child, new_parent, target, new_name);

    if (status < 0) {
        return status == -1 ? -EIO : (int)status;
    }

    return 0;
}

static int _split_path(const char *path, char **dir_out, char **base_out) {
    if (!path || !dir_out || !base_out) {
        return -EINVAL;
    }

    *dir_out = NULL;
    *base_out = NULL;

    const char *slash = strrchr(path, '/');

    if (!slash) {
        *dir_out = strdup(".");
        *base_out = strdup(path);
    } else if (slash == path) {
        if (slash[1] == '\0') {
            return -EINVAL;
        }

        *dir_out = strdup("/");
        *base_out = strdup(slash + 1);
    } else {
        size_t dir_len = (size_t)(slash - path);
        char *dir = malloc(dir_len + 1);

        if (!dir) {
            return -ENOMEM;
        }

        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';

        *dir_out = dir;
        *base_out = strdup(slash + 1);
    }

    if (!*dir_out || !*base_out) {
        free(*dir_out);
        free(*base_out);
        *dir_out = NULL;
        *base_out = NULL;
        return -ENOMEM;
    }

    if ((*base_out)[0] == '\0') {
        free(*dir_out);
        free(*base_out);
        *dir_out = NULL;
        *base_out = NULL;
        return -EINVAL;
    }

    return 0;
}

static int parent_error(vfs_node_t *parent) {
    if (parent) {
        return -ENOTDIR;
    }

    if (errno) {
        return -errno;
    }

    return -ENOENT;
}

static vfs_node_t *_walk_from_locked(vfs_node_t *from, const char *path, size_t *depth) {
    tree_node_t *node = from ? from->tree_entry : NULL;
    if (path && path[0] == '/') {
        node = vfs && vfs->tree ? vfs->tree->root : NULL;
    }

    if (!node) {
        errno = ENXIO;
        return NULL;
    }

    char *tok_pos = NULL;
    char *tok_str = strdup(path);

    if (!tok_str) {
        errno = ENOMEM;
        return NULL;
    }

    char *pos = strtok_r(tok_str, "/", &tok_pos);

    while (pos) {
        if (!strcmp(pos, ".")) {
            goto next;
        }

        if (!strcmp(pos, "..")) {
            if (node->parent) {
                node = node->parent;
            }

            goto next;
        }

        vfs_node_t *parent = node->data;

        if (VFS_IS_LINK(parent->type)) {
            parent = _follow_link_locked(parent, depth);

            if (!parent) {
                node = NULL;
                break;
            }

            node = parent->tree_entry;
        }

        tree_node_t *next = NULL;
        if (!_find_child(parent, pos, &next) || !next) {
            node = NULL;
            errno = ENOENT;
            break;
        }

        node = next;

    next:
        pos = strtok_r(NULL, "/", &tok_pos);
    }

    free(tok_str);
    return node ? node->data : NULL;
}

static vfs_node_t *_lookup_from_locked(vfs_node_t *from, const char *path) {
    vfs_node_t *cached = _path_cache_get(path);
    if (cached) {
        return cached;
    }

    size_t depth = 0;
    vfs_node_t *node = _walk_from_locked(from, path, &depth);

    if (node) {
        _path_cache_put(path, node);
    }

    return node;
}

static vfs_node_t *_lookup_locked(const char *path) {
    tree_node_t *root = vfs->tree->root;
    return _lookup_from_locked(root ? root->data : NULL, path);
}

static int _parent_base_common(const char *path, vfs_node_t **parent_out, char **base_out, bool locked) {
    if (!path || !parent_out || !base_out) {
        return -EINVAL;
    }

    char *dir_name = NULL;
    char *base_name = NULL;

    int status = _split_path(path, &dir_name, &base_name);
    if (status < 0) {
        return status;
    }

    errno = 0;
    vfs_node_t *parent = locked ? _lookup_locked(dir_name) : vfs_lookup(dir_name);
    free(dir_name);

    if (parent && VFS_IS_LINK(parent->type)) {
        if (locked) {
            size_t depth = 0;
            parent = _follow_link_locked(parent, &depth);
        } else {
            parent = _follow_link(parent);
        }
    }

    if (!parent || parent->type != VFS_DIR) {
        free(base_name);
        return parent_error(parent);
    }

    *parent_out = parent;
    *base_out = base_name;
    return 0;
}

static int _parent_base(const char *path, vfs_node_t **parent_out, char **base_out) {
    return _parent_base_common(path, parent_out, base_out, false);
}

static int _parent_base_locked(const char *path, vfs_node_t **parent_out, char **base_out) {
    return _parent_base_common(path, parent_out, base_out, true);
}

static int _rename_lookup(const char *old_path, const char *new_path, rename_op_t *op) {
    int status = _parent_base_locked(old_path, &op->old_parent, &op->old_base);
    if (status < 0) {
        return status;
    }

    status = _parent_base_locked(new_path, &op->new_parent, &op->new_base);
    if (status < 0) {
        return status;
    }

    op->child = _find_child(op->old_parent, op->old_base, &op->child_entry);
    if (!op->child || !op->child_entry) {
        return -ENOENT;
    }

    if (!vfs_validate_name(op->new_base)) {
        return -EBADF;
    }

    op->target = _find_any_child(op->new_parent, op->new_base, NULL);
    if (op->target == op->child) {
        return 1;
    }

    bool same_inode = op->target && op->child->fs && op->child->fs == op->target->fs;
    same_inode = same_inode && op->child->inode && op->child->inode == op->target->inode;
    if (same_inode) {
        return 1;
    }

    status = check_target(op->child, op->target);
    if (status != 0) {
        return status < 0 ? status : 1;
    }

    bool moves_dir_into_child = op->child->type == VFS_DIR;
    moves_dir_into_child = moves_dir_into_child && _is_ancestor(op->child_entry, op->new_parent->tree_entry);
    if (moves_dir_into_child) {
        return -EINVAL;
    }

    op->new_name = strdup(op->new_base);
    return op->new_name ? 0 : -ENOMEM;
}

static int _hold_parent_base(const char *path, vfs_node_t **parent_out, char **base_out) {
    if (!parent_out || !base_out) {
        return -EINVAL;
    }

    mutex_lock(&vfs->lock);

    vfs_node_t *parent = NULL;
    char *base = NULL;
    int status = _parent_base_locked(path, &parent, &base);
    if (status < 0) {
        mutex_unlock(&vfs->lock);
        return status;
    }

    if (parent->removed) {
        free(base);
        mutex_unlock(&vfs->lock);
        return -ENOENT;
    }

    vfs_node_retain(parent);
    mutex_unlock(&vfs->lock);

    *parent_out = parent;
    *base_out = base;
    return 0;
}

static int _hold_resolved(const char *path, vfs_node_t **node_out) {
    if (!path || !node_out) {
        return -EINVAL;
    }

    mutex_lock(&vfs->lock);

    errno = 0;
    vfs_node_t *node = _lookup_locked(path);
    if (node && VFS_IS_LINK(node->type)) {
        size_t depth = 0;
        node = _follow_link_locked(node, &depth);
    }

    if (!node) {
        int status = errno ? -errno : -ENOENT;
        mutex_unlock(&vfs->lock);
        return status;
    }

    if (node->removed) {
        mutex_unlock(&vfs->lock);
        return -ENOENT;
    }

    vfs_node_retain(node);
    mutex_unlock(&vfs->lock);

    *node_out = node;
    return 0;
}

vfs_t *vfs_init(void) {
    vfs = calloc(1, sizeof(vfs_t));
    assert(vfs);
    vfs->lock = (mutex_t)MUTEX_INIT;

    vfs_node_t *root = vfs_create_node(NULL, VFS_DIR);
    assert(root);

    vfs->tree = tree_create_rooted(root->tree_entry);
    assert(vfs->tree);

    log_debug("vfs ready");
    return vfs;
}

vfs_node_t *vfs_create_node(char *name, u32 type) {
    vfs_node_t *node = calloc(1, sizeof(vfs_node_t));

    if (!node) {
        return NULL;
    }

    node->type = type;
    node->tree_entry = tree_create_node(node);
    if (!node->tree_entry) {
        free(node);
        return NULL;
    }

    node->time.created = _time_now();
    node->time.modified = node->time.created;
    node->time.accessed = node->time.created;
    node->nlink = 1;

    if (name) {
        node->name = strdup(name);
        if (!node->name) {
            tree_destroy_node(node->tree_entry);
            free(node);
            return NULL;
        }
    }

    return node;
}

void vfs_destroy_node(vfs_node_t *node) {
    if (!node) {
        return;
    }

    if (node->tree_entry) {
        tree_destroy_node(node->tree_entry);
        node->tree_entry = NULL;
    }

    _free_node_data(node);
}

void vfs_node_retain(vfs_node_t *node) {
    if (!node) {
        return;
    }

    __atomic_fetch_add(&node->refs, 1, __ATOMIC_ACQ_REL);
}

void vfs_node_release(vfs_node_t *node) {
    if (!node) {
        return;
    }

    u32 old_refs = __atomic_fetch_sub(&node->refs, 1, __ATOMIC_ACQ_REL);
    if (!old_refs) {
        panic("vfs node release underflow");
    }

    if (old_refs != 1 || !node->removed) {
        return;
    }

    mutex_lock(&vfs->lock);
    if (node->removed && node->tree_entry && !node->tree_entry->parent &&
        __atomic_load_n(&node->refs, __ATOMIC_ACQUIRE) == 0) {
        tree_prune_callback(node->tree_entry, _free_tree_node);
    }
    mutex_unlock(&vfs->lock);
}

void vfs_node_open(vfs_node_t *node) {
    if (!node) {
        return;
    }

    vfs_node_retain(node);
    __atomic_fetch_add(&node->open_refs, 1, __ATOMIC_ACQ_REL);
}

void vfs_node_close(vfs_node_t *node) {
    if (!node) {
        return;
    }

    u32 old_refs = __atomic_fetch_sub(&node->open_refs, 1, __ATOMIC_ACQ_REL);
    if (!old_refs) {
        panic("vfs node close underflow");
    }

    vfs_node_release(node);
}

vfs_interface_t *vfs_create_interface(vfs_io_fn read, vfs_io_fn write, vfs_truncate_fn truncate) {
    vfs_interface_t *interface = calloc(1, sizeof(vfs_interface_t));

    if (!interface) {
        return NULL;
    }

    interface->refcount = 1;
    interface->read = read;
    interface->write = write;
    interface->truncate = truncate;

    return interface;
}

void vfs_destroy_interface(vfs_interface_t *interface) {
    if (!interface || interface->refcount == VFS_INTERFACE_STATIC) {
        return;
    }

    u32 old_refs = __atomic_fetch_sub(&interface->refcount, 1, __ATOMIC_ACQ_REL);
    if (!old_refs) {
        panic("vfs interface release underflow");
    }

    if (old_refs == 1) {
        free(interface);
    }
}

static void _hold_interface(vfs_interface_t *interface) {
    if (!interface || interface->refcount == VFS_INTERFACE_STATIC) {
        return;
    }

    __atomic_fetch_add(&interface->refcount, 1, __ATOMIC_ACQ_REL);
}

void vfs_clear_interface(vfs_node_t *node) {
    if (!node || !node->interface) {
        return;
    }

    vfs_interface_t *old = node->interface;
    node->interface = NULL;
    vfs_destroy_interface(old);
}

void vfs_set_interface(vfs_node_t *node, vfs_interface_t *interface) {
    if (!node) {
        return;
    }

    if (node->interface == interface) {
        return;
    }

    _hold_interface(interface);
    vfs_clear_interface(node);
    node->interface = interface;
}

void vfs_make_virtual(vfs_node_t *node) {
    if (!node) {
        return;
    }

    fs_interface_t *node_iface = NULL;
    if (node->fs && node->fs->filesystem) {
        node_iface = node->fs->filesystem->node_interface;
    }

    if (node_iface && node_iface->destroy_node) {
        node_iface->destroy_node(node->fs, node);
    }

    vfs_clear_interface(node);

    if (node->symlink_target) {
        free(node->symlink_target);
        node->symlink_target = NULL;
    }

    node->link = NULL;
    node->private = NULL;
    node->fs = NULL;
    node->inode = 0;
    node->uid = 0;
    node->gid = 0;
    node->size = 0;
    node->nlink = 1;
    _stamp_virtual_node(node);
}

void vfs_adopt_interface(vfs_node_t *node, vfs_interface_t *interface) {
    if (!node) {
        vfs_destroy_interface(interface);
        return;
    }

    if (node->interface == interface) {
        return;
    }

    vfs_clear_interface(node);
    node->interface = interface;
}

bool vfs_validate_name(const char *name) {
    if (!name || !name[0]) {
        return false;
    }

    if (!strcmp(name, ".")) {
        return false;
    }

    if (!strcmp(name, "..")) {
        return false;
    }

    return !strchr(name, '/');
}

vfs_node_t *vfs_lookup_from(vfs_node_t *from, const char *path) {
    assert(vfs);

    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    mutex_lock(&vfs->lock);
    vfs_node_t *result = _lookup_from_locked(from, path);
    mutex_unlock(&vfs->lock);
    return result;
}

vfs_node_t *vfs_lookup(const char *path) {
    assert(vfs);

    tree_node_t *root = vfs->tree->root;
    return vfs_lookup_from(root->data, path);
}

vfs_node_t *vfs_lookup_hold(const char *path, bool follow_links) {
    assert(vfs);

    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    mutex_lock(&vfs->lock);

    vfs_node_t *node = _lookup_locked(path);
    if (node && follow_links) {
        size_t depth = 0;
        node = _follow_link_locked(node, &depth);
    }

    if (node && !node->removed && !node->busy) {
        vfs_node_retain(node);
    } else {
        node = NULL;
    }

    mutex_unlock(&vfs->lock);
    return node;
}

vfs_node_t *vfs_lookup_relative(const char *root, const char *path) {
    vfs_node_t *vnode_root = vfs_lookup(root);

    if (!vnode_root) {
        return NULL;
    }

    return vfs_lookup_from(vnode_root, path);
}

vfs_node_t *vfs_open(const char *path, u32 type, bool create, mode_t mode) {
    vfs_node_t *file = vfs_lookup(path);

    if (file) {
        return file;
    }

    if (!create) {
        return NULL;
    }

    vfs_node_t *parent = NULL;
    char *base_name = NULL;

    if (_parent_base(path, &parent, &base_name) < 0) {
        return NULL;
    }

    vfs_node_t *child = vfs_create(parent, base_name, type, mode);
    free(base_name);

    return child;
}

vfs_node_t *vfs_resolve_node(vfs_node_t *node) {
    return _follow_link(node);
}

static bool _node_access(vfs_node_t *vnode, uid_t uid, gid_t gid, int mode) {
    int perm = 0;

    if (!vnode) {
        return false;
    }

    if (!uid) {
        mode_t execute = S_IXUSR | S_IXGRP | S_IXOTH;
        if ((mode & X_OK) && vnode->type == VFS_FILE && !(vnode->mode & execute)) {
            return false;
        }

        return true;
    }

    if (uid == vnode->uid) {
        if (mode & R_OK) {
            perm |= (vnode->mode & S_IRUSR) ? R_OK : 0;
        }
        if (mode & W_OK) {
            perm |= (vnode->mode & S_IWUSR) ? W_OK : 0;
        }
        if (mode & X_OK) {
            perm |= (vnode->mode & S_IXUSR) ? X_OK : 0;
        }
    } else if (sched_gid_matches_cred(uid, gid, vnode->gid)) {
        if (mode & R_OK) {
            perm |= (vnode->mode & S_IRGRP) ? R_OK : 0;
        }
        if (mode & W_OK) {
            perm |= (vnode->mode & S_IWGRP) ? W_OK : 0;
        }
        if (mode & X_OK) {
            perm |= (vnode->mode & S_IXGRP) ? X_OK : 0;
        }
    } else {
        if (mode & R_OK) {
            perm |= (vnode->mode & S_IROTH) ? R_OK : 0;
        }
        if (mode & W_OK) {
            perm |= (vnode->mode & S_IWOTH) ? W_OK : 0;
        }
        if (mode & X_OK) {
            perm |= (vnode->mode & S_IXOTH) ? X_OK : 0;
        }
    }

    return (perm & mode) == mode;
}

int vfs_access(vfs_node_t *vnode, uid_t uid, gid_t gid, int mode) {
    if (!vnode) {
        return -ENOENT;
    }

    errno = 0;
    vnode = _follow_link(vnode);

    if (!vnode) {
        return errno ? -errno : -ENOENT;
    }

    return _node_access(vnode, uid, gid, mode) ? 0 : -EACCES;
}

int vfs_check_search(const char *path, uid_t uid, gid_t gid, bool allow_missing_leaf) {
    if (!path || !path[0]) {
        return -EINVAL;
    }

    int status = 0;
    char *copy = NULL;

    if (strcmp(path, "/")) {
        copy = strdup(path);
        if (!copy) {
            return -ENOMEM;
        }
    }

    mutex_lock(&vfs->lock);

    tree_node_t *root = vfs && vfs->tree ? vfs->tree->root : NULL;
    vfs_node_t *current = root ? root->data : NULL;

    if (!current) {
        mutex_unlock(&vfs->lock);
        free(copy);
        return -ENOENT;
    }

    if (!strcmp(path, "/")) {
        size_t depth = 0;
        current = _follow_link_locked(current, &depth);

        if (!current) {
            mutex_unlock(&vfs->lock);
            return errno ? -errno : -ENOENT;
        }

        if (current->type != VFS_DIR) {
            mutex_unlock(&vfs->lock);
            return -ENOTDIR;
        }

        status = _node_access(current, uid, gid, X_OK) ? 0 : -EACCES;
        mutex_unlock(&vfs->lock);
        return status;
    }

    size_t depth = 0;
    char *save = NULL;
    char *segment = strtok_r(copy, "/", &save);

    while (segment) {
        char *next_segment = strtok_r(NULL, "/", &save);
        current = _follow_link_locked(current, &depth);

        if (!current) {
            status = -ENOENT;
            break;
        }

        if (current->type != VFS_DIR) {
            status = -ENOTDIR;
            break;
        }

        if (!_node_access(current, uid, gid, X_OK)) {
            status = -EACCES;
            break;
        }

        vfs_node_t *next = _find_child(current, segment, NULL);
        if (!next) {
            if (allow_missing_leaf && !next_segment) {
                status = 0;
            } else {
                status = -ENOENT;
            }
            break;
        }

        current = next;
        segment = next_segment;
    }

    free(copy);
    mutex_unlock(&vfs->lock);
    return status;
}

int vfs_stat_node(vfs_node_t *node, stat_t *out, bool follow_links) {
    if (!node || !out) {
        return -EINVAL;
    }

    errno = 0;
    node = _follow_mounts(node);

    if (follow_links) {
        errno = 0;
        node = _follow_link(node);
    }

    if (!node) {
        return errno ? -errno : -ENOENT;
    }

    memset(out, 0, sizeof(*out));

    out->st_ino = node->inode;
    out->st_mode = _type_mode(node->type) | (node->mode & ~S_IFMT);
    out->st_nlink = node->nlink;
    if (node->removed && !node->fs) {
        out->st_nlink = 0;
    } else if (!node->removed && !out->st_nlink) {
        out->st_nlink = 1;
    }
    out->st_uid = node->uid;
    out->st_gid = node->gid;
    out->st_size = (off_t)node->size;
    out->st_blksize = 512;
    out->st_blocks = (blkcnt_t)((node->size + 511) / 512);
    out->st_atime = node->time.accessed;
    out->st_mtime = node->time.modified;
    out->st_ctime = node->time.created;

    return 0;
}

int vfs_chmod(vfs_node_t *node, mode_t mode) {
    if (!node) {
        return -ENOENT;
    }

    errno = 0;
    node = _follow_link(node);
    if (!node) {
        return errno ? -errno : -ENOENT;
    }

    mode_t desired = mode & 07777;
    fs_t *filesystem = node->fs ? node->fs->filesystem : NULL;

    if (filesystem) {
        fs_interface_t *iface = NULL;

        if (filesystem->node_interface && filesystem->node_interface->chmod) {
            iface = filesystem->node_interface;
        } else if (filesystem->fs_interface && filesystem->fs_interface->chmod) {
            iface = filesystem->fs_interface;
        }

        if (iface && !iface->chmod(node->fs, node, desired)) {
            return -EIO;
        }
    }

    node->mode = desired;

    if (!node->fs) {
        node->time.created = _time_now();
    }

    return 0;
}

int vfs_chown(vfs_node_t *node, uid_t uid, gid_t gid) {
    if (!node) {
        return -ENOENT;
    }

    errno = 0;
    node = _follow_link(node);
    if (!node) {
        return errno ? -errno : -ENOENT;
    }

    fs_t *filesystem = node->fs ? node->fs->filesystem : NULL;

    if (filesystem) {
        fs_interface_t *iface = NULL;

        if (filesystem->node_interface && filesystem->node_interface->chown) {
            iface = filesystem->node_interface;
        } else if (filesystem->fs_interface && filesystem->fs_interface->chown) {
            iface = filesystem->fs_interface;
        }

        if (iface && !iface->chown(node->fs, node, uid, gid)) {
            return -EIO;
        }
    }

    node->uid = uid;
    node->gid = gid;

    if (!node->fs) {
        node->time.created = _time_now();
    }

    return 0;
}

int vfs_set_times(vfs_node_t *node, time_t atime, time_t mtime) {
    if (!node) {
        return -ENOENT;
    }

    errno = 0;
    node = _follow_link(node);
    if (!node) {
        return errno ? -errno : -ENOENT;
    }

    time_t ctime = _time_now();
    fs_t *filesystem = node->fs ? node->fs->filesystem : NULL;

    if (filesystem) {
        fs_interface_t *iface = NULL;

        if (filesystem->node_interface && filesystem->node_interface->set_times) {
            iface = filesystem->node_interface;
        } else if (filesystem->fs_interface && filesystem->fs_interface->set_times) {
            iface = filesystem->fs_interface;
        }

        if (!iface) {
            return -ENOTSUP;
        }

        int status = iface->set_times(node->fs, node, atime, mtime, ctime);
        if (status < 0) {
            return status;
        }
    }

    node->time.accessed = atime;
    node->time.modified = mtime;
    node->time.created = ctime;
    return 0;
}

static int _insert_child(vfs_node_t *parent, vfs_node_t *child, bool persist, bool hold_child) {
    assert(vfs);
    mutex_lock(&vfs->lock);

    if (!parent || !child) {
        mutex_unlock(&vfs->lock);
        return -EINVAL;
    }

    if (!vfs_validate_name(child->name)) {
        mutex_unlock(&vfs->lock);
        return -EBADF;
    }

    if (VFS_IS_LINK(parent->type)) {
        size_t depth = 0;
        parent = _follow_link_locked(parent, &depth);

        if (!parent) {
            mutex_unlock(&vfs->lock);
            return -EINVAL;
        }
    }

    if (parent->removed) {
        mutex_unlock(&vfs->lock);
        return -ENOENT;
    }

    tree_node_t *parent_entry = parent->tree_entry;

    assert(parent_entry);
    assert(child->tree_entry);

    if (_find_any_child(parent, child->name, NULL)) {
        mutex_unlock(&vfs->lock);
        return -EEXIST;
    }

    if (!tree_insert_child(parent_entry, child->tree_entry)) {
        mutex_unlock(&vfs->lock);
        return -ENOMEM;
    }

    bool held_parent = false;
    vfs_interface_t *interface = parent->interface;
    if (persist && interface && interface->create) {
        child->busy = true;
        vfs_node_retain(parent);
        held_parent = true;
        _hold_interface(interface);
        mutex_unlock(&vfs->lock);

        ssize_t fs_ret = interface->create(parent, child);
        if (fs_ret < 0) {
            vfs_destroy_interface(interface);

            mutex_lock(&vfs->lock);
            tree_remove_child(parent_entry, child->tree_entry);
            child->tree_entry->parent = NULL;
            child->busy = false;
            mutex_unlock(&vfs->lock);
            vfs_node_release(parent);
            return fs_ret == -1 ? -EIO : (int)fs_ret;
        }

        vfs_destroy_interface(interface);

        mutex_lock(&vfs->lock);
        child->busy = false;
    }

    _child_index_set(parent, child->name, child->tree_entry);
    _path_cache_clear();

    if (hold_child) {
        vfs_node_retain(child);
    }

    mutex_unlock(&vfs->lock);

    if (held_parent) {
        vfs_node_release(parent);
    }

    return 0;
}

static int _link_child(vfs_node_t *parent, vfs_node_t *child, vfs_node_t *target) {
    assert(vfs);
    mutex_lock(&vfs->lock);

    if (!parent || !child || !target) {
        mutex_unlock(&vfs->lock);
        return -EINVAL;
    }

    if (!vfs_validate_name(child->name)) {
        mutex_unlock(&vfs->lock);
        return -EBADF;
    }

    if (VFS_IS_LINK(parent->type)) {
        size_t depth = 0;
        parent = _follow_link_locked(parent, &depth);

        if (!parent) {
            mutex_unlock(&vfs->lock);
            return -EINVAL;
        }
    }

    if (parent->removed) {
        mutex_unlock(&vfs->lock);
        return -ENOENT;
    }

    if (target->removed || target->type == VFS_DIR || target->type == VFS_MOUNT) {
        mutex_unlock(&vfs->lock);
        return -EPERM;
    }

    vfs_interface_t *interface = parent->interface;
    if (!interface || !interface->link) {
        mutex_unlock(&vfs->lock);
        return -ENOTSUP;
    }

    if (parent->fs != target->fs) {
        mutex_unlock(&vfs->lock);
        return -EXDEV;
    }

    tree_node_t *parent_entry = parent->tree_entry;

    assert(parent_entry);
    assert(child->tree_entry);

    if (_find_any_child(parent, child->name, NULL)) {
        mutex_unlock(&vfs->lock);
        return -EEXIST;
    }

    if (!tree_insert_child(parent_entry, child->tree_entry)) {
        mutex_unlock(&vfs->lock);
        return -ENOMEM;
    }

    child->busy = true;
    vfs_node_retain(parent);
    vfs_node_retain(target);
    _hold_interface(interface);
    mutex_unlock(&vfs->lock);

    ssize_t link_ret = interface->link(parent, child, target);
    vfs_destroy_interface(interface);

    mutex_lock(&vfs->lock);
    child->busy = false;

    if (link_ret < 0) {
        tree_remove_child(parent_entry, child->tree_entry);
        child->tree_entry->parent = NULL;
        mutex_unlock(&vfs->lock);
        vfs_node_release(target);
        vfs_node_release(parent);
        return link_ret == -1 ? -EIO : (int)link_ret;
    }

    _child_index_set(parent, child->name, child->tree_entry);
    _path_cache_clear();
    mutex_unlock(&vfs->lock);

    vfs_node_release(target);
    vfs_node_release(parent);
    return 0;
}

static int _symlink_path(const char *target, const char *link_path) {
    if (!target || !target[0] || !link_path) {
        return -ENOENT;
    }

    vfs_node_t *parent = NULL;
    char *base_name = NULL;
    int status = _hold_parent_base(link_path, &parent, &base_name);
    if (status < 0) {
        return status;
    }

    if (!vfs_validate_name(base_name)) {
        free(base_name);
        vfs_node_release(parent);
        return -EBADF;
    }

    vfs_node_t *link = vfs_create_node(base_name, VFS_SYMLINK);
    free(base_name);

    if (!link) {
        vfs_node_release(parent);
        return -ENOMEM;
    }

    link->symlink_target = strdup(target);
    if (!link->symlink_target) {
        vfs_destroy_node(link);
        vfs_node_release(parent);
        return -ENOMEM;
    }

    link->mode = 0777;
    link->size = strlen(target);

    status = _insert_child(parent, link, true, false);
    if (status < 0) {
        vfs_destroy_node(link);
        vfs_node_release(parent);
        return status;
    }

    vfs_node_release(parent);
    return 0;
}

int vfs_symlink(const char *target, const char *link_path) {
    return _symlink_path(target, link_path);
}

static int _hardlink_path(const char *target, const char *link_path) {
    if (!target || !link_path) {
        return -EINVAL;
    }

    vfs_node_t *target_node = NULL;
    int status = _hold_resolved(target, &target_node);
    if (status < 0) {
        return status;
    }

    if (target_node->type == VFS_DIR || target_node->type == VFS_MOUNT) {
        vfs_node_release(target_node);
        return -EPERM;
    }

    vfs_node_t *parent = NULL;
    char *base_name = NULL;
    status = _hold_parent_base(link_path, &parent, &base_name);
    if (status < 0) {
        vfs_node_release(target_node);
        return status;
    }

    if (!vfs_validate_name(base_name)) {
        free(base_name);
        vfs_node_release(parent);
        vfs_node_release(target_node);
        return -EBADF;
    }

    vfs_node_t *link = vfs_create_node(base_name, target_node->type);
    free(base_name);

    if (!link) {
        vfs_node_release(parent);
        vfs_node_release(target_node);
        return -ENOMEM;
    }

    link->mode = target_node->mode;
    link->uid = target_node->uid;
    link->gid = target_node->gid;
    link->inode = target_node->inode;
    link->size = target_node->size;
    link->nlink = target_node->nlink;
    link->time = target_node->time;

    status = _link_child(parent, link, target_node);
    if (status < 0) {
        vfs_destroy_node(link);
        vfs_node_release(parent);
        vfs_node_release(target_node);
        return status;
    }

    vfs_node_release(parent);
    vfs_node_release(target_node);
    return 0;
}

int vfs_hardlink(const char *target, const char *link_path) {
    return _hardlink_path(target, link_path);
}

static int _remove_path(const char *path, bool dir) {
    if (!path) {
        return -EINVAL;
    }

    mutex_lock(&vfs->lock);

    vfs_node_t *parent = NULL;
    char *base_name = NULL;
    int status = _parent_base_locked(path, &parent, &base_name);
    if (status < 0) {
        mutex_unlock(&vfs->lock);
        return status;
    }

    tree_node_t *child_entry = NULL;
    vfs_node_t *child = _find_child(parent, base_name, &child_entry);
    free(base_name);

    if (!child || !child_entry) {
        mutex_unlock(&vfs->lock);
        return -ENOENT;
    }

    if (dir) {
        if (child->type != VFS_DIR) {
            mutex_unlock(&vfs->lock);
            return -ENOTDIR;
        }

        bool has_entry = child->tree_entry != NULL;
        bool has_children = has_entry && child->tree_entry->children && child->tree_entry->children->length;
        bool empty_dir = has_entry && !has_children;
        if (!empty_dir) {
            mutex_unlock(&vfs->lock);
            return -ENOTEMPTY;
        }
    } else if (child->type == VFS_DIR || child->type == VFS_MOUNT) {
        mutex_unlock(&vfs->lock);
        return -EISDIR;
    }

    vfs_interface_t *interface = parent->interface;
    if (parent->interface && parent->interface->remove) {
        child->busy = true;
        vfs_node_retain(parent);
        vfs_node_retain(child);
        _hold_interface(interface);
        mutex_unlock(&vfs->lock);

        ssize_t fs_ret = interface->remove(parent, child);
        vfs_destroy_interface(interface);

        mutex_lock(&vfs->lock);
        child->busy = false;

        if (fs_ret < 0) {
            mutex_unlock(&vfs->lock);
            vfs_node_release(child);
            vfs_node_release(parent);
            return fs_ret == -1 ? -EIO : (int)fs_ret;
        }
    }

    status = _remove_child(parent, child);
    mutex_unlock(&vfs->lock);

    if (interface && interface->remove) {
        vfs_node_release(child);
        vfs_node_release(parent);
    }

    return status;
}

int vfs_unlink(const char *path) {
    return _remove_path(path, false);
}

int vfs_rmdir(const char *path) {
    return _remove_path(path, true);
}

int vfs_detach_child(vfs_node_t *parent, vfs_node_t *child) {
    if (!parent || !child) {
        return -EINVAL;
    }

    mutex_lock(&vfs->lock);

    if (VFS_IS_LINK(parent->type)) {
        size_t depth = 0;
        parent = _follow_link_locked(parent, &depth);
    }

    if (!parent) {
        mutex_unlock(&vfs->lock);
        return -ENOENT;
    }

    int status = _detach_child(parent, child);
    mutex_unlock(&vfs->lock);
    return status;
}

int vfs_rename(const char *old_path, const char *new_path) {
    if (!old_path || !new_path) {
        return -EINVAL;
    }

    mutex_lock(&vfs->lock);

    rename_op_t op = { 0 };
    int status = _rename_lookup(old_path, new_path, &op);
    if (status == 1) {
        status = 0;
        goto out;
    }

    if (status < 0) {
        goto out;
    }

    status = _rename_fs_node(op.old_parent, op.child, op.new_parent, op.target, op.new_base);
    if (status == 0) {
        status = _rename_tree(&op);
    }

out:
    _rename_op_clear(&op);
    mutex_unlock(&vfs->lock);
    return status;
}

int vfs_insert_child(vfs_node_t *parent, vfs_node_t *child) {
    return _insert_child(parent, child, true, false);
}

int vfs_insert_virtual(vfs_node_t *parent, vfs_node_t *child) {
    return _insert_child(parent, child, false, false);
}

static vfs_node_t *_create_node(vfs_node_t *parent, char *name, u32 type, mode_t mode, bool hold) {
    assert(vfs);

    if (!parent) {
        return NULL;
    }

    vfs_node_t *node = vfs_create_node(name, type);

    if (!node) {
        return NULL;
    }

    node->mode = mode;

    if (_insert_child(parent, node, true, hold) < 0) {
        vfs_destroy_node(node);
        return NULL;
    }

    return node;
}

vfs_node_t *vfs_create(vfs_node_t *parent, char *name, u32 type, mode_t mode) {
    return _create_node(parent, name, type, mode, false);
}

vfs_node_t *vfs_create_hold(vfs_node_t *parent, char *name, u32 type, mode_t mode) {
    return _create_node(parent, name, type, mode, true);
}

vfs_node_t *vfs_create_virtual(vfs_node_t *parent, char *name, u32 type, mode_t mode) {
    assert(vfs);

    if (!parent) {
        return NULL;
    }

    vfs_node_t *node = vfs_create_node(name, type);

    if (!node) {
        return NULL;
    }

    node->mode = mode;
    _stamp_virtual_node(node);

    if (vfs_insert_virtual(parent, node) < 0) {
        vfs_destroy_node(node);
        return NULL;
    }

    return node;
}

int vfs_mount(fs_instance_t *instance, vfs_node_t *mount) {
    assert(vfs);

    if (!instance || !mount) {
        log_warn("vfs mount failed: invalid instance or mount");
        return -EINVAL;
    }

    if (mount->type != VFS_DIR) {
        log_warn("vfs mount failed: mount point type=%lu", (unsigned long)mount->type);
        return -ENOTDIR;
    }

    if (!instance->has_tree) {
        if (!instance->filesystem) {
            log_warn("vfs mount failed: instance has no filesystem");
            return -EINVAL;
        }

        fs_interface_t *interface = instance->filesystem->fs_interface;

        if (!interface || !interface->build_tree) {
            log_warn("vfs mount failed: filesystem %s cannot build tree", instance->filesystem->name);
            return -ENOTSUP;
        }

        if (!interface->build_tree(instance)) {
            log_warn("vfs mount failed: filesystem %s tree build failed", instance->filesystem->name);
            return -EIO;
        }

        if (!instance->has_tree || !instance->subtree_root) {
            log_warn("vfs mount failed: filesystem %s produced no root", instance->filesystem->name);
            return -EIO;
        }
    }

    // the mount point remains in the parent tree and redirects lookups to the fs root
    mutex_lock(&vfs->lock);
    vfs_node_t *target = instance->subtree_root->data;
    if (!target || _mount_cycle(mount, target)) {
        log_warn("vfs mount failed: invalid target or mount cycle");
        mutex_unlock(&vfs->lock);
        return -ELOOP;
    }

    mount->type = VFS_MOUNT;
    mount->link = target;
    instance->refcount++;
    _path_cache_clear();
    mutex_unlock(&vfs->lock);

    log_debug("mounted %s", mount->name ? mount->name : "/");
    return 0;
}

int vfs_unmount(vfs_node_t *mount, bool destroy_tree) {
    assert(vfs);

    if (!mount) {
        return -EINVAL;
    }

    mutex_lock(&vfs->lock);

    if (mount->type != VFS_MOUNT) {
        mutex_unlock(&vfs->lock);
        return -EINVAL;
    }

    vfs_node_t *link = mount->link;

    if (!link) {
        mutex_unlock(&vfs->lock);
        return -ENOENT;
    }

    fs_instance_t *instance = link->fs;

    if (!instance) {
        mutex_unlock(&vfs->lock);
        return -EINVAL;
    }

    if (!instance->refcount) {
        mutex_unlock(&vfs->lock);
        return -EINVAL;
    }

    // mounted trees can be shared, so teardown waits until the last mount goes away
    bool last_mount = instance->refcount == 1;
    if (last_mount && destroy_tree && _tree_has_refs(instance->subtree_root)) {
        mutex_unlock(&vfs->lock);
        return -EBUSY;
    }

    instance->refcount--;

    if (!instance->refcount && destroy_tree && instance->filesystem) {
        fs_interface_t *interface = instance->filesystem->fs_interface;

        if (interface && interface->destroy_tree) {
            if (!interface->destroy_tree(instance)) {
                instance->refcount++;
                mutex_unlock(&vfs->lock);
                return -EBUSY;
            }
        }
    }

    mount->type = VFS_DIR;
    mount->link = NULL;
    _path_cache_clear();
    mutex_unlock(&vfs->lock);

    log_debug("unmounted %s", mount->name ? mount->name : "/");
    return 0;
}

static void _dump_recursive(tree_node_t *parent, size_t depth) {
    vfs_node_t *vnode_parent = parent->data;

    if (VFS_IS_LINK(vnode_parent->type) && vnode_parent->link) {
        parent = vnode_parent->link->tree_entry;
    }

    ll_foreach(node, parent->children) {
        tree_node_t *child = node->data;
        vfs_node_t *vnode = child->data;

        log_debug("%-*s|- %s ", (int)depth, "", vnode->name);

        _dump_recursive(child, depth + 1);
    }
}

void dump_vfs(void) {
    assert(vfs);

    log_debug("vfs tree:");
    _dump_recursive(vfs->tree->root, 0);
}

ssize_t vfs_read(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags) {
    errno = 0;
    node = _follow_link(node);
    if (!node) {
        return errno ? -errno : -ENOENT;
    }

    if (!node->interface || !node->interface->read) {
        return -ENOTSUP;
    }

    ssize_t bytes = node->interface->read(node, buf, offset, len, flags);

    if (bytes >= 0 && !node->fs) {
        node->time.accessed = _time_now();
    }

    return bytes;
}

ssize_t vfs_write(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags) {
    errno = 0;
    node = _follow_link(node);
    if (!node) {
        return errno ? -errno : -ENOENT;
    }

    if (!node->interface || !node->interface->write) {
        return -ENOTSUP;
    }

    ssize_t bytes = node->interface->write(node, buf, offset, len, flags);
    if (bytes >= 0 && !node->fs) {
        time_t now = _time_now();

        node->time.accessed = now;
        node->time.modified = now;
        node->time.created = now;
    }
    return bytes;
}

ssize_t vfs_truncate(vfs_node_t *node, size_t len) {
    errno = 0;
    node = _follow_link(node);
    if (!node) {
        return errno ? -errno : -ENOENT;
    }

    if (!node->interface || !node->interface->truncate) {
        return -ENOTSUP;
    }

    ssize_t status = node->interface->truncate(node, len);
    if (status >= 0 && !node->fs) {
        time_t now = _time_now();

        node->time.modified = now;
        node->time.created = now;
    }
    return status;
}

ssize_t vfs_mmap(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags) {
    errno = 0;
    node = _follow_link(node);
    if (!node) {
        return errno ? -errno : -ENOENT;
    }

    if (!VFS_IS_DEVICE(node->type)) {
        return -ENODEV;
    }

    if (!node->interface || !node->interface->mmap) {
        return -ENOTSUP;
    }

    return node->interface->mmap(node, buf, offset, len, flags);
}

ssize_t vfs_ioctl(vfs_node_t *node, u64 request, void *args) {
    errno = 0;
    node = _follow_link(node);
    if (!node) {
        return errno ? -errno : -ENOENT;
    }

    if (!VFS_IS_DEVICE(node->type)) {
        return -ENOTTY;
    }

    if (!node->interface || !node->interface->ioctl) {
        return -ENOTTY;
    }

    return node->interface->ioctl(node, request, args);
}

static bool poll_read_ready(enum vfs_node_type type) {
    switch (type) {
    case VFS_FILE:
    case VFS_DIR:
    case VFS_CHARDEV:
    case VFS_BLOCKDEV:
        return true;
    default:
        return false;
    }
}

static bool poll_write_ready(enum vfs_node_type type) {
    switch (type) {
    case VFS_FILE:
    case VFS_CHARDEV:
    case VFS_BLOCKDEV:
        return true;
    default:
        return false;
    }
}

short vfs_poll(vfs_node_t *node, short events, size_t flags) {
    errno = 0;
    node = _follow_link(node);
    if (!node) {
        return errno ? -errno : -ENODEV;
    }

    if (node->interface && node->interface->poll) {
        return node->interface->poll(node, events, (u32)flags);
    }

    short revents = 0;

    if ((events & POLLIN) && poll_read_ready(node->type)) {
        revents |= POLLIN;
    }

    if ((events & POLLOUT) && poll_write_ready(node->type)) {
        revents |= POLLOUT;
    }

    (void)flags;
    return revents;
}

struct sched_wait_queue *vfs_wait_queue(vfs_node_t *node, short events, size_t flags) {
    errno = 0;
    node = _follow_link(node);
    if (!node || !node->interface || !node->interface->wait_queue) {
        return NULL;
    }

    return node->interface->wait_queue(node, events, (u32)flags);
}
