#include "vfs.h"

#include <data/list.h>
#include <errno.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "panic.h"


static vfs_t* vfs = NULL;


static char* _strdup(const char* src) {
    if (!src)
        return NULL;

    size_t len = strlen(src);
    char* out = malloc(len + 1);

    if (!out)
        return NULL;

    memcpy(out, src, len);
    out[len] = '\0';

    return out;
}

static bool _split_path(const char* path, char** dir_out, char** base_out) {
    if (!path || !dir_out || !base_out)
        return false;

    *dir_out = NULL;
    *base_out = NULL;

    const char* slash = strrchr(path, '/');

    if (!slash) {
        *dir_out = _strdup(".");
        *base_out = _strdup(path);
    } else if (slash == path) {
        if (slash[1] == '\0')
            return false;

        *dir_out = _strdup("/");
        *base_out = _strdup(slash + 1);
    } else {
        size_t dir_len = (size_t)(slash - path);
        char* dir = malloc(dir_len + 1);

        if (!dir)
            return false;

        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';

        *dir_out = dir;
        *base_out = _strdup(slash + 1);
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


vfs_t* vfs_init(void) {
    vfs = calloc(1, sizeof(vfs_t));
    assert(vfs);

    vfs_node_t* root = vfs_create_node(NULL, VFS_DIR);
    vfs->tree = tree_create_rooted(root->tree_entry);

    log_info("vfs: initialized");
    return vfs;
}


vfs_node_t* vfs_create_node(char* name, u32 type) {
    vfs_node_t* node = calloc(1, sizeof(vfs_node_t));

    if (!node)
        return NULL;

    node->type = type;
    node->tree_entry = tree_create_node(node);

    if (name)
        node->name = _strdup(name);

    return node;
}

void vfs_destroy_node(vfs_node_t* node) {
    if (!node)
        return;

    if (node->interface)
        free(node->interface);

    if (node->name)
        free(node->name);

    if (node->tree_entry)
        tree_destroy_node(node->tree_entry);

    free(node);
}


vfs_interface_t* vfs_create_interface(
    ssize_t (*read)(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags),
    ssize_t (*write)(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags)
) {
    vfs_interface_t* interface = calloc(1, sizeof(vfs_interface_t));

    if (!interface)
        return NULL;

    interface->read = read;
    interface->write = write;

    return interface;
}

void vfs_destroy_interface(vfs_interface_t* interface) {
    free(interface);
}


bool vfs_validate_name(const char* name) {
    if (!name || !name[0])
        return false;

    if (!strcmp(name, "."))
        return false;

    if (!strcmp(name, ".."))
        return false;

    return !strchr(name, '/');
}


vfs_node_t* vfs_lookup_from(vfs_node_t* from, const char* path) {
    assert(vfs);

    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    tree_node_t* node = from ? from->tree_entry : NULL;

    if (!node) {
        errno = ENXIO;
        return NULL;
    }

    char* tok_pos = NULL;
    char* tok_str = _strdup(path);

    if (!tok_str) {
        errno = ENOMEM;
        return NULL;
    }

    char* pos = strtok_r(tok_str, "/", &tok_pos);

    while (pos) {
        if (!strcmp(pos, "."))
            goto next;

        if (!strcmp(pos, "..")) {
            if (node->parent)
                node = node->parent;

            goto next;
        }

        vfs_node_t* parent = node->data;

        if (VFS_IS_LINK(parent->type)) {
            parent = parent->link;

            if (!parent) {
                node = NULL;
                errno = ENOENT;
                break;
            }

            node = parent->tree_entry;
        }

        bool found = false;

        ll_foreach(child, node->children) {
            tree_node_t* tnode = child->data;
            vfs_node_t* vnode = tnode->data;

            if (!strcmp(vnode->name, pos)) {
                found = true;
                node = tnode;
                break;
            }
        }

        if (!found) {
            node = NULL;
            errno = ENOENT;
            break;
        }

    next:
        pos = strtok_r(NULL, "/", &tok_pos);
    }

    free(tok_str);

    if (!node)
        return NULL;

    return node->data;
}

vfs_node_t* vfs_lookup(const char* path) {
    assert(vfs);

    tree_node_t* root = vfs->tree->root;
    return vfs_lookup_from(root->data, path);
}

vfs_node_t* vfs_lookup_relative(const char* root, const char* path) {
    vfs_node_t* vnode_root = vfs_lookup(root);

    if (!vnode_root)
        return NULL;

    return vfs_lookup_from(vnode_root, path);
}

vfs_node_t* vfs_open(const char* path, u32 type, bool create, mode_t mode) {
    vfs_node_t* file = vfs_lookup(path);

    if (file)
        return file;

    if (!create)
        return NULL;

    char* dir_name = NULL;
    char* base_name = NULL;

    if (!_split_path(path, &dir_name, &base_name))
        return NULL;

    vfs_node_t* parent = vfs_lookup(dir_name);
    vfs_node_t* child = vfs_create(parent, base_name, type, mode);

    free(dir_name);
    free(base_name);

    return child;
}

bool vfs_access(vfs_node_t* vnode, uid_t uid, gid_t gid, int mode) {
    int perm = 0;

    if (!vnode)
        return false;

    if (uid == vnode->uid) {
        if (mode & R_OK)
            perm |= (vnode->mode & S_IRUSR) ? R_OK : 0;
        if (mode & W_OK)
            perm |= (vnode->mode & S_IWUSR) ? W_OK : 0;
        if (mode & X_OK)
            perm |= (vnode->mode & S_IXUSR) ? X_OK : 0;
    } else if (gid == vnode->gid) {
        if (mode & R_OK)
            perm |= (vnode->mode & S_IRGRP) ? R_OK : 0;
        if (mode & W_OK)
            perm |= (vnode->mode & S_IWGRP) ? W_OK : 0;
        if (mode & X_OK)
            perm |= (vnode->mode & S_IXGRP) ? X_OK : 0;
    } else {
        if (mode & R_OK)
            perm |= (vnode->mode & S_IROTH) ? R_OK : 0;
        if (mode & W_OK)
            perm |= (vnode->mode & S_IWOTH) ? W_OK : 0;
        if (mode & X_OK)
            perm |= (vnode->mode & S_IXOTH) ? X_OK : 0;
    }

    return (perm & mode) == mode;
}


bool vfs_insert_child(vfs_node_t* parent, vfs_node_t* child) {
    assert(vfs);

    if (!parent || !child) {
        errno = EINVAL;
        return false;
    }

    if (!vfs_validate_name(child->name)) {
        errno = EBADF;
        log_warn("vfs: invalid child name");
        return false;
    }

    if (VFS_IS_LINK(parent->type)) {
        parent = parent->link;

        if (!parent) {
            errno = EINVAL;
            log_warn("vfs: link target missing during insert");
            return false;
        }
    }

    tree_node_t* parent_tnode = parent->tree_entry;

    assert(parent_tnode);
    assert(child->tree_entry);

    ll_foreach(node, parent_tnode->children) {
        tree_node_t* child_tnode = node->data;
        vfs_node_t* child_vnode = child_tnode->data;

        if (!strcmp(child_vnode->name, child->name)) {
            errno = EEXIST;
            log_warn("vfs: duplicate name '%s'", child->name);
            return false;
        }
    }

    tree_insert_child(parent_tnode, child->tree_entry);

    vfs_interface_t* interface = parent->interface;

    if (interface && interface->create)
        interface->create(parent, child);

    return true;
}

vfs_node_t* vfs_create(vfs_node_t* parent, char* name, u32 type, mode_t mode) {
    assert(vfs);

    if (!parent)
        return NULL;

    vfs_node_t* node = vfs_create_node(name, type);

    if (!node)
        return NULL;

    node->mode = mode;

    if (!vfs_insert_child(parent, node)) {
        vfs_destroy_node(node);
        return NULL;
    }

    return node;
}


bool vfs_mount(fs_instance_t* instance, vfs_node_t* mount) {
    assert(vfs);

    if (!instance || !mount)
        return false;

    if (mount->type != VFS_DIR)
        return false;

    if (!instance->has_tree) {
        if (!instance->fs)
            return false;

        fs_interface_t* interface = instance->fs->fs_interface;

        if (!interface || !interface->build_tree)
            return false;

        if (!interface->build_tree(instance))
            return false;

        if (!instance->has_tree || !instance->subtree_root)
            return false;
    }

    mount->type = VFS_MOUNT;
    mount->link = instance->subtree_root->data;

    instance->refcount++;

    log_info("vfs: mounted '%s'", mount->name ? mount->name : "/");
    return true;
}

bool vfs_unmount(vfs_node_t* mount, bool destroy_tree) {
    assert(vfs);

    if (mount->type != VFS_MOUNT)
        return false;

    vfs_node_t* link = mount->link;

    if (!link)
        return false;

    fs_instance_t* instance = link->fs;

    if (!instance)
        return false;

    if (!instance->refcount)
        return false;

    instance->refcount--;

    if (!instance->refcount && destroy_tree && instance->fs) {
        fs_interface_t* interface = instance->fs->fs_interface;

        if (interface && interface->destroy_tree)
            interface->destroy_tree(instance);
    }

    mount->type = VFS_DIR;
    mount->link = NULL;

    log_info("vfs: unmounted '%s'", mount->name ? mount->name : "/");
    return true;
}


static void _dump_recursive(tree_node_t* parent, size_t depth) {
    vfs_node_t* vnode_parent = parent->data;

    if (VFS_IS_LINK(vnode_parent->type))
        parent = vnode_parent->link->tree_entry;

    ll_foreach(node, parent->children) {
        tree_node_t* child = node->data;
        vfs_node_t* vnode = child->data;

        log_debug("%-*s|- %s ", (int)depth, "", vnode->name);

        _dump_recursive(child, depth + 1);
    }
}

void dump_vfs(void) {
    assert(vfs);

    log_debug("Recursive dump of the virtual file system:");

    _dump_recursive(vfs->tree->root, 0);
}

ssize_t vfs_read(vfs_node_t* node, void* buf, size_t offset, size_t len, size_t flags) {
    if (!node)
        return -1;

    if (!node->interface || !node->interface->read)
        return -1;

    return node->interface->read(node, buf, offset, len, flags);
}

ssize_t vfs_write(vfs_node_t* node, void* buf, size_t offset, size_t len, size_t flags) {
    if (!node)
        return -1;

    if (!node->interface || !node->interface->write)
        return -1;

    return node->interface->write(node, buf, offset, len, flags);
}

ssize_t vfs_mmap(vfs_node_t* node, void* buf, size_t offset, size_t len, size_t flags) {
    if (!node)
        return -1;

    if (!VFS_IS_DEVICE(node->type))
        return -1;

    if (!node->interface || !node->interface->mmap)
        return -1;

    return node->interface->mmap(node, buf, offset, len, flags);
}

ssize_t vfs_ioctl(vfs_node_t* node, u64 request, void* args) {
    if (!node)
        return -1;

    if (!VFS_IS_DEVICE(node->type))
        return -1;

    if (!node->interface || !node->interface->ioctl)
        return -1;

    return node->interface->ioctl(node, request, args);
}
