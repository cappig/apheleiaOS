#include "vfs.h"

#include <arch/arch.h>
#include <data/hashmap.h>
#include <data/list.h>
#include <errno.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "panic.h"


static vfs_t *vfs = NULL;

static time_t _time_now(void) {
    return (time_t)arch_wallclock_seconds();
}

static vfs_node_t *_resolve_link(vfs_node_t *node) {
    if (!node) {
        return NULL;
    }

    if (node->type == VFS_MOUNT && node->link) {
        return node->link;
    }

    if (node->type == VFS_SYMLINK) {
        if (!node->link && node->symlink_target) {
            node->link = vfs_lookup(node->symlink_target);
        }

        return node->link;
    }

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

static bool _child_index_get(
    vfs_node_t *parent,
    const char *name,
    tree_node_t **tnode_out
) {
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

static void
_child_index_set(vfs_node_t *parent, const char *name, tree_node_t *tnode) {
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

    u64 encoded = 0;
    if (!hashmap_str_get(map, name, &encoded)) {
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

    if (node->interface) {
        free(node->interface);
    }

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

static vfs_node_t *
_find_child(vfs_node_t *parent, const char *name, tree_node_t **out_tnode) {
    if (!parent || !name || !parent->tree_entry) {
        return NULL;
    }

    tree_node_t *indexed = NULL;
    if (_child_index_get(parent, name, &indexed) && indexed && indexed->data) {
        if (out_tnode) {
            *out_tnode = indexed;
        }

        return indexed->data;
    }

    ll_foreach(child, parent->tree_entry->children) {
        tree_node_t *tnode = child->data;
        vfs_node_t *vnode = tnode ? tnode->data : NULL;

        if (!vnode || !vnode->name) {
            continue;
        }

        if (!strcmp(vnode->name, name)) {
            _child_index_set(parent, vnode->name, tnode);
            if (out_tnode) {
                *out_tnode = tnode;
            }

            return vnode;
        }
    }

    return NULL;
}

static bool _remove_child(vfs_node_t *parent, vfs_node_t *child) {
    if (!parent || !child || !parent->tree_entry || !child->tree_entry) {
        return false;
    }

    if (!tree_remove_child(parent->tree_entry, child->tree_entry)) {
        return false;
    }

    _child_index_remove(parent, child->name);
    tree_prune_callback(child->tree_entry, _free_tree_node);
    return true;
}

static bool _split_path(const char *path, char **dir_out, char **base_out) {
    if (!path || !dir_out || !base_out) {
        return false;
    }

    *dir_out = NULL;
    *base_out = NULL;

    const char *slash = strrchr(path, '/');

    if (!slash) {
        *dir_out = strdup(".");
        *base_out = strdup(path);
    } else if (slash == path) {
        if (slash[1] == '\0') {
            return false;
        }

        *dir_out = strdup("/");
        *base_out = strdup(slash + 1);
    } else {
        size_t dir_len = (size_t)(slash - path);
        char *dir = malloc(dir_len + 1);

        if (!dir) {
            return false;
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
        return false;
    }

    if ((*base_out)[0] == '\0') {
        free(*dir_out);
        free(*base_out);
        *dir_out = NULL;
        *base_out = NULL;
        return false;
    }

    return true;
}

static bool _resolve_parent_base(
    const char *path,
    vfs_node_t **parent_out,
    char **base_out
) {
    if (!path || !parent_out || !base_out) {
        return false;
    }

    char *dir_name = NULL;
    char *base_name = NULL;

    if (!_split_path(path, &dir_name, &base_name)) {
        return false;
    }

    vfs_node_t *parent = vfs_lookup(dir_name);
    free(dir_name);

    if (parent && VFS_IS_LINK(parent->type)) {
        parent = parent->link;
    }

    if (!parent || parent->type != VFS_DIR) {
        free(base_name);
        return false;
    }

    *parent_out = parent;
    *base_out = base_name;
    return true;
}


vfs_t *vfs_init(void) {
    vfs = calloc(1, sizeof(vfs_t));
    assert(vfs);

    vfs_node_t *root = vfs_create_node(NULL, VFS_DIR);
    vfs->tree = tree_create_rooted(root->tree_entry);

    log_debug("VFS initialized");
    return vfs;
}


vfs_node_t *vfs_create_node(char *name, u32 type) {
    vfs_node_t *node = calloc(1, sizeof(vfs_node_t));

    if (!node) {
        return NULL;
    }

    node->type = type;
    node->tree_entry = tree_create_node(node);
    node->time.created = _time_now();
    node->time.modified = node->time.created;
    node->time.accessed = node->time.created;

    if (name) {
        node->name = strdup(name);
    }

    return node;
}

void vfs_destroy_node(vfs_node_t *node) {
    if (!node) {
        return;
    }

    _child_index_clear(node);

    if (node->interface) {
        free(node->interface);
    }

    if (node->name) {
        free(node->name);
    }
    if (node->symlink_target) {
        free(node->symlink_target);
    }

    if (node->tree_entry) {
        tree_destroy_node(node->tree_entry);
    }

    free(node);
}


vfs_interface_t *vfs_create_interface(
    ssize_t (*read)(
        vfs_node_t *node,
        void *buf,
        size_t offset,
        size_t len,
        u32 flags
    ),
    ssize_t (*write)(
        vfs_node_t *node,
        void *buf,
        size_t offset,
        size_t len,
        u32 flags
    ),
    ssize_t (*truncate)(vfs_node_t *node, size_t len)
) {
    vfs_interface_t *interface = calloc(1, sizeof(vfs_interface_t));

    if (!interface) {
        return NULL;
    }

    interface->read = read;
    interface->write = write;
    interface->truncate = truncate;

    return interface;
}

void vfs_destroy_interface(vfs_interface_t *interface) {
    free(interface);
}


bool _validate_name(const char *name) {
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

    tree_node_t *node = from ? from->tree_entry : NULL;

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
            parent = parent->link;

            if (!parent) {
                node = NULL;
                errno = ENOENT;
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

    if (!node) {
        return NULL;
    }

    return node->data;
}

vfs_node_t *vfs_lookup(const char *path) {
    assert(vfs);

    tree_node_t *root = vfs->tree->root;
    return vfs_lookup_from(root->data, path);
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

    if (!_resolve_parent_base(path, &parent, &base_name)) {
        return NULL;
    }

    vfs_node_t *child = vfs_create(parent, base_name, type, mode);
    free(base_name);

    return child;
}

bool vfs_access(vfs_node_t *vnode, uid_t uid, gid_t gid, int mode) {
    int perm = 0;

    if (!vnode) {
        return false;
    }

    if (!uid) {
        return true;
    }

    vnode = _resolve_link(vnode);

    if (!vnode) {
        return false;
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

int vfs_check_search(
    const char *path,
    uid_t uid,
    gid_t gid,
    bool allow_missing_leaf
) {
    if (!path || !path[0]) {
        return -EINVAL;
    }

    vfs_node_t *current = vfs_lookup("/");
    current = _resolve_link(current);
    if (!current) {
        return -ENOENT;
    }

    if (!strcmp(path, "/")) {
        if (current->type != VFS_DIR) {
            return -ENOTDIR;
        }

        return vfs_access(current, uid, gid, X_OK) ? 0 : -EACCES;
    }

    char *copy = strdup(path);
    if (!copy) {
        return -ENOMEM;
    }

    int ret = 0;
    char *save = NULL;
    char *segment = strtok_r(copy, "/", &save);

    while (segment) {
        char *next_segment = strtok_r(NULL, "/", &save);
        current = _resolve_link(current);

        if (!current) {
            ret = -ENOENT;
            break;
        }

        if (current->type != VFS_DIR) {
            ret = -ENOTDIR;
            break;
        }

        if (!vfs_access(current, uid, gid, X_OK)) {
            ret = -EACCES;
            break;
        }

        vfs_node_t *next = _find_child(current, segment, NULL);
        if (!next) {
            if (allow_missing_leaf && !next_segment) {
                ret = 0;
            } else {
                ret = -ENOENT;
            }
            break;
        }

        current = next;
        segment = next_segment;
    }

    free(copy);
    return ret;
}

bool vfs_stat_node(vfs_node_t *node, stat_t *out, bool follow_links) {
    if (!node || !out) {
        return false;
    }

    if (follow_links) {
        node = _resolve_link(node);
    }

    if (!node) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    out->st_ino = node->inode;
    out->st_mode = _type_mode(node->type) | (node->mode & ~S_IFMT);
    out->st_nlink = 1;
    out->st_uid = node->uid;
    out->st_gid = node->gid;
    out->st_size = (off_t)node->size;
    out->st_atime = node->time.accessed;
    out->st_mtime = node->time.modified;
    out->st_ctime = node->time.created;

    return true;
}

bool vfs_chmod(vfs_node_t *node, mode_t mode) {
    if (!node) {
        return false;
    }

    node = _resolve_link(node);
    if (!node) {
        return false;
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
            return false;
        }
    }

    node->mode = desired;

    if (!node->fs) {
        node->time.created = _time_now();
    }

    return true;
}

bool vfs_chown(vfs_node_t *node, uid_t uid, gid_t gid) {
    if (!node) {
        return false;
    }

    node = _resolve_link(node);
    if (!node) {
        return false;
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
            return false;
        }
    }

    node->uid = uid;
    node->gid = gid;

    if (!node->fs) {
        node->time.created = _time_now();
    }

    return true;
}

bool vfs_link(const char *target, const char *link_path) {
    if (!target || !link_path) {
        return false;
    }

    vfs_node_t *target_node = vfs_lookup(target);

    vfs_node_t *parent = NULL;
    char *base_name = NULL;
    if (!_resolve_parent_base(link_path, &parent, &base_name)) {
        return false;
    }

    if (!_validate_name(base_name) || _find_child(parent, base_name, NULL)) {
        free(base_name);
        return false;
    }

    vfs_node_t *link = vfs_create_node(base_name, VFS_SYMLINK);
    free(base_name);

    if (!link) {
        return false;
    }

    link->symlink_target = strdup(target);
    if (!link->symlink_target) {
        vfs_destroy_node(link);
        return false;
    }

    link->link = target_node;
    link->mode = 0777;
    link->size = strlen(target);

    if (target_node) {
        link->inode = target_node->inode;
        link->uid = target_node->uid;
        link->gid = target_node->gid;
        link->time = target_node->time;
    }

    if (!vfs_insert_child(parent, link)) {
        vfs_destroy_node(link);
        return false;
    }

    return true;
}

bool vfs_unlink(const char *path) {
    if (!path) {
        return false;
    }

    vfs_node_t *parent = NULL;
    char *base_name = NULL;
    if (!_resolve_parent_base(path, &parent, &base_name)) {
        return false;
    }

    tree_node_t *child_tnode = NULL;
    vfs_node_t *child = _find_child(parent, base_name, &child_tnode);
    free(base_name);

    if (!child || !child_tnode) {
        return false;
    }

    if (child->type == VFS_DIR || child->type == VFS_MOUNT) {
        return false;
    }

    if (parent->interface && parent->interface->remove) {
        if (parent->interface->remove(parent, child->name) < 0) {
            return false;
        }
    }

    return _remove_child(parent, child);
}

bool vfs_rmdir(const char *path) {
    if (!path) {
        return false;
    }

    vfs_node_t *parent = NULL;
    char *base_name = NULL;
    if (!_resolve_parent_base(path, &parent, &base_name)) {
        return false;
    }

    tree_node_t *child_tnode = NULL;
    vfs_node_t *child = _find_child(parent, base_name, &child_tnode);
    free(base_name);

    if (!child || !child_tnode) {
        return false;
    }

    if (child->type != VFS_DIR) {
        return false;
    }

    if (
        !child->tree_entry ||
        (child->tree_entry->children && child->tree_entry->children->length)
    ) {
        return false;
    }

    if (parent->interface && parent->interface->remove) {
        if (parent->interface->remove(parent, child->name) < 0) {
            return false;
        }
    }

    return _remove_child(parent, child);
}

bool vfs_rename(const char *old_path, const char *new_path) {
    if (!old_path || !new_path) {
        return false;
    }

    vfs_node_t *old_parent = NULL;
    char *old_base = NULL;
    vfs_node_t *new_parent = NULL;
    char *new_base = NULL;

    if (!_resolve_parent_base(old_path, &old_parent, &old_base)) {
        return false;
    }

    if (!_resolve_parent_base(new_path, &new_parent, &new_base)) {
        free(old_base);
        return false;
    }

    tree_node_t *child_tnode = NULL;
    vfs_node_t *child = _find_child(old_parent, old_base, &child_tnode);

    if (!child || !child_tnode) {
        free(old_base);
        free(new_base);
        return false;
    }

    if (!_validate_name(new_base) || _find_child(new_parent, new_base, NULL)) {
        free(old_base);
        free(new_base);
        return false;
    }

    if (!tree_remove_child(old_parent->tree_entry, child_tnode)) {
        free(old_base);
        free(new_base);
        return false;
    }
    _child_index_remove(old_parent, old_base);

    char *new_name = strdup(new_base);

    free(old_base);
    free(new_base);

    if (!new_name) {
        tree_insert_child(old_parent->tree_entry, child_tnode);
        _child_index_set(old_parent, child->name, child_tnode);
        return false;
    }

    free(child->name);
    child->name = new_name;

    tree_insert_child(new_parent->tree_entry, child_tnode);
    _child_index_set(new_parent, child->name, child_tnode);

    return true;
}


static bool _vfs_insert_child(vfs_node_t *parent, vfs_node_t *child, bool persist) {
    assert(vfs);

    if (!parent || !child) {
        errno = EINVAL;
        return false;
    }

    if (!_validate_name(child->name)) {
        errno = EBADF;
        return false;
    }

    if (VFS_IS_LINK(parent->type)) {
        parent = parent->link;

        if (!parent) {
            errno = EINVAL;
            return false;
        }
    }

    tree_node_t *parent_tnode = parent->tree_entry;

    assert(parent_tnode);
    assert(child->tree_entry);

    if (_find_child(parent, child->name, NULL)) {
        errno = EEXIST;
        return false;
    }

    tree_insert_child(parent_tnode, child->tree_entry);

    vfs_interface_t *interface = parent->interface;

    if (persist && interface && interface->create) {
        if (interface->create(parent, child) < 0) {
            tree_remove_child(parent_tnode, child->tree_entry);
            errno = EIO;
            return false;
        }
    }

    _child_index_set(parent, child->name, child->tree_entry);
    return true;
}

bool vfs_insert_child(vfs_node_t *parent, vfs_node_t *child) {
    return _vfs_insert_child(parent, child, true);
}

bool vfs_insert_child_virtual(vfs_node_t *parent, vfs_node_t *child) {
    return _vfs_insert_child(parent, child, false);
}

vfs_node_t *vfs_create(vfs_node_t *parent, char *name, u32 type, mode_t mode) {
    assert(vfs);

    if (!parent) {
        return NULL;
    }

    vfs_node_t *node = vfs_create_node(name, type);

    if (!node) {
        return NULL;
    }

    node->mode = mode;

    if (!vfs_insert_child(parent, node)) {
        vfs_destroy_node(node);
        return NULL;
    }

    return node;
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

    if (!vfs_insert_child_virtual(parent, node)) {
        vfs_destroy_node(node);
        return NULL;
    }

    return node;
}


bool vfs_mount(fs_instance_t *instance, vfs_node_t *mount) {
    assert(vfs);

    if (!instance || !mount) {
        return false;
    }

    if (mount->type != VFS_DIR) {
        return false;
    }

    if (!instance->has_tree) {
        if (!instance->filesystem) {
            return false;
        }

        fs_interface_t *interface = instance->filesystem->fs_interface;

        if (!interface || !interface->build_tree) {
            return false;
        }

        if (!interface->build_tree(instance)) {
            return false;
        }

        if (!instance->has_tree || !instance->subtree_root) {
            return false;
        }
    }

    mount->type = VFS_MOUNT;
    mount->link = instance->subtree_root->data;

    instance->refcount++;

    log_debug("mounted '%s'", mount->name ? mount->name : "/");
    return true;
}

bool vfs_unmount(vfs_node_t *mount, bool destroy_tree) {
    assert(vfs);

    if (mount->type != VFS_MOUNT) {
        return false;
    }

    vfs_node_t *link = mount->link;

    if (!link) {
        return false;
    }

    fs_instance_t *instance = link->fs;

    if (!instance) {
        return false;
    }

    if (!instance->refcount) {
        return false;
    }

    instance->refcount--;

    if (!instance->refcount && destroy_tree && instance->filesystem) {
        fs_interface_t *interface = instance->filesystem->fs_interface;

        if (interface && interface->destroy_tree) {
            interface->destroy_tree(instance);
        }
    }

    mount->type = VFS_DIR;
    mount->link = NULL;

    log_debug("unmounted '%s'", mount->name ? mount->name : "/");
    return true;
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

    log_debug("Recursive dump of the virtual file system:");

    _dump_recursive(vfs->tree->root, 0);
}

ssize_t
vfs_read(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags) {
    node = _resolve_link(node);
    if (!node) {
        return -1;
    }

    if (!node->interface || !node->interface->read) {
        return -1;
    }

    ssize_t rc = node->interface->read(node, buf, offset, len, flags);

    if (rc >= 0 && !node->fs) {
        node->time.accessed = _time_now();
    }

    return rc;
}

ssize_t vfs_write(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    size_t flags
) {
    node = _resolve_link(node);
    if (!node) {
        return -1;
    }

    if (!node->interface || !node->interface->write) {
        return -1;
    }

    ssize_t rc = node->interface->write(node, buf, offset, len, flags);
    if (rc >= 0 && !node->fs) {
        time_t now = _time_now();

        node->time.accessed = now;
        node->time.modified = now;
        node->time.created = now;
    }
    return rc;
}

ssize_t vfs_truncate(vfs_node_t *node, size_t len) {
    node = _resolve_link(node);
    if (!node) {
        return -1;
    }

    if (!node->interface || !node->interface->truncate) {
        return -1;
    }

    ssize_t rc = node->interface->truncate(node, len);
    if (rc >= 0 && !node->fs) {
        time_t now = _time_now();

        node->time.modified = now;
        node->time.created = now;
    }
    return rc;
}

ssize_t
vfs_mmap(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags) {
    node = _resolve_link(node);
    if (!node) {
        return -1;
    }

    if (!VFS_IS_DEVICE(node->type)) {
        return -1;
    }

    if (!node->interface || !node->interface->mmap) {
        return -1;
    }

    return node->interface->mmap(node, buf, offset, len, flags);
}

ssize_t vfs_ioctl(vfs_node_t *node, u64 request, void *args) {
    node = _resolve_link(node);
    if (!node) {
        return -1;
    }

    if (!VFS_IS_DEVICE(node->type)) {
        return -1;
    }

    if (!node->interface || !node->interface->ioctl) {
        return -1;
    }

    return node->interface->ioctl(node, request, args);
}

short vfs_poll(vfs_node_t *node, short events, size_t flags) {
    node = _resolve_link(node);
    if (!node) {
        return -ENODEV;
    }

    if (node->interface && node->interface->poll) {
        return node->interface->poll(node, events, (u32)flags);
    }

    short revents = 0;

    if (events & POLLIN) {
        if (
            node->type == VFS_FILE ||
            node->type == VFS_DIR ||
            node->type == VFS_CHARDEV ||
            node->type == VFS_BLOCKDEV
        ) {
            revents |= POLLIN;
        }
    }

    if (events & POLLOUT) {
        if (node->type == VFS_FILE || node->type == VFS_CHARDEV || node->type == VFS_BLOCKDEV) {
            revents |= POLLOUT;
        }
    }

    (void)flags;
    return revents;
}
