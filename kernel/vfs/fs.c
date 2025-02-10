#include "fs.h"

#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>
#include <errno.h>
#include <log/log.h>
#include <string.h>

#include "libc_ext/stdlib.h"
#include "mem/heap.h"
#include "sys/disk.h"
#include "sys/panic.h"

// Our kernel has one single virtual file system
virtual_fs* vfs = NULL;


virtual_fs* vfs_init() {
    vfs = kcalloc(sizeof(virtual_fs));
    assert(vfs);

    vfs_node* root = vfs_create_node(NULL, VFS_DIR);
    vfs->tree = tree_create_rooted(root->tree_entry);

    /* vfs_node* dev = vfs_create_node("dev", VFS_DIR); */
    /* vfs_node* mnt = vfs_create_node("mnt", VFS_DIR); */
    /**/
    /* vfs_insert_child(root, dev); */
    /* vfs_insert_child(root, mnt); */

    return vfs;
}


vfs_node* vfs_create_node(char* name, vfs_node_type type) {
    vfs_node* node = kcalloc(sizeof(vfs_node));
    node->type = type;

    node->tree_entry = tree_create_node(node);

    if (name)
        node->name = strdup(name);

    // TODO: timestamp

    return node;
}

void vfs_destroy_node(vfs_node* node) {
    if (node->interface)
        kfree(node->interface);

    if (node->name)
        kfree(node->name);

    if (node->tree_entry)
        tree_destroy_node(node->tree_entry);

    kfree(node);
}


vfs_node_interface* vfs_create_interface(vfs_read_fn read, vfs_write_fn write) {
    vfs_node_interface* interface = kcalloc(sizeof(vfs_node_interface));
    interface->read = read;
    interface->write = write;

    return interface;
}

void vfs_destroy_interface(vfs_node_interface* interface) {
    kfree(interface);
}


bool vfs_validate_name(const char* name) {
    if (!strcmp(name, "."))
        return false;

    if (!strcmp(name, ".."))
        return false;

    return !strchr(name, '/');
}


vfs_node* vfs_lookup_from(vfs_node* from, const char* path) {
    assert(vfs);

    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    tree_node* node = from->tree_entry;

    if (!node) {
        errno = ENXIO;
        return NULL;
    }

    char* tok_pos = NULL;
    char* tok_str = strdup(path);
    char* pos = strtok_r(tok_str, "/", &tok_pos);

    while (pos) {
        // Stay at the same level
        if (!strcmp(pos, "."))
            goto next;

        // Go one level up
        if (!strcmp(pos, "..")) {
            if (node->parent)
                node = node->parent;

            goto next;
        }

        // Resolve symbolic links
        vfs_node* parent = node->data;
        if (VFS_IS_LINK(parent->type)) {
            parent = parent->link;

            if (!parent) {
                node = NULL;
                errno = ENOENT;

                break;
            }

            node = parent->tree_entry;
        }

        // Go one level down
        bool found = false;

        foreach (child, node->children) {
            tree_node* tnode = child->data;
            vfs_node* vnode = tnode->data;

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

    kfree(tok_str);

    if (!node)
        return NULL;

    return node->data;
}

vfs_node* vfs_lookup(const char* path) {
    assert(vfs);

    tree_node* root = vfs->tree->root;
    return vfs_lookup_from(root->data, path);
}

vfs_node* vfs_lookup_relative(const char* root, const char* path) {
    vfs_node* vnode_root = vfs_lookup(root);

    if (!vnode_root)
        return NULL;

    return vfs_lookup_from(vnode_root, path);
}

// 'mode' is only relevant if the node gets created
vfs_node* vfs_open(const char* path, vfs_node_type type, bool create, vfs_mode mode) {
    vfs_node* file = vfs_lookup(path);

    if (file)
        return file;

    // The file doesn't exist
    if (!create)
        return NULL;

    char* dir_cpy = strdup(path);
    char* dir_name = dirname(dir_cpy);

    char* base_cpy = strdup(path);
    char* base_name = basename(base_cpy);

    vfs_node* parent = vfs_lookup(dir_name);
    vfs_node* child = vfs_create(parent, base_name, type, mode);

    kfree(dir_cpy);
    kfree(base_cpy);

    return child;
}


bool vfs_insert_child(vfs_node* parent, vfs_node* child) {
    assert(vfs);

    if (!parent || !child) {
        errno = EINVAL;
        return false;
    }

    if (!vfs_validate_name(child->name)) {
        errno = EBADF;
        return false;
    }

    // Resolve links
    if (VFS_IS_LINK(parent->type)) {
        parent = parent->link;

        if (!parent) {
            errno = EINVAL;
            return false;
        }
    }

    tree_node* parent_tnode = parent->tree_entry;

    assert(parent_tnode);
    assert(child->tree_entry);

    // file names are unique inside a given folder
    foreach (node, parent_tnode->children) {
        tree_node* child_tnode = node->data;
        vfs_node* child_vnode = child_tnode->data;

        if (!strcmp(child_vnode->name, child->name)) {
            errno = EEXIST;
            return false;
        }
    }

    tree_insert_child(parent_tnode, child->tree_entry);

    vfs_node_interface* interface = parent->interface;

    if (interface && interface->create)
        interface->create(parent, child);

    return true;
}

vfs_node* vfs_create(vfs_node* parent, char* name, vfs_node_type type, vfs_mode mode) {
    assert(vfs);

    if (!parent)
        return NULL;

    vfs_node* node = vfs_create_node(name, type);

    if (!vfs_insert_child(parent, node)) {
        vfs_destroy_node(node);
        return NULL;
    }

    return node;
}


bool vfs_mount(file_system_instance* instance, vfs_node* mount) {
    assert(vfs);

    if (!instance || !mount)
        return false;

    if (mount->type != VFS_DIR)
        return false;

    // Do we have to build the subtree
    if (!instance->tree_built) {
        if (!instance->fs)
            return false;

        file_system_interface* interface = instance->fs->fs_interface;

        if (!interface || !interface->build_tree)
            return false;

        interface->build_tree(instance);

        if (!instance->tree_built)
            return false;
    }

    mount->type = VFS_MOUNT;
    mount->link = instance->subtree->root->data;

    instance->refcount++;

    return true;
}

// If the refcount goes to 0 the tree will be destroyed in 'destroy_tree' is true
bool vfs_unmount(vfs_node* mount, bool destroy_tree) {
    assert(vfs);

    if (mount->type != VFS_MOUNT)
        return false;

    vfs_node* link = mount->link;

    if (!link)
        return false;

    file_system_instance* instance = link->fs;

    if (!instance)
        return false;

    if (!instance->refcount) // wtf?
        return false;

    instance->refcount--;

    if (!instance->refcount && destroy_tree && instance->fs) {
        file_system_interface* interface = instance->fs->fs_interface;

        if (interface && interface->destroy_tree)
            interface->destroy_tree(instance);
    }

    mount->type = VFS_DIR;
    mount->link = NULL;

    return true;
}


static void _recursive_dump(tree_node* parent, usize depth) {
    vfs_node* vnode_parent = parent->data;

    if (VFS_IS_LINK(vnode_parent->type))
        parent = vnode_parent->link->tree_entry;

    foreach (node, parent->children) {
        tree_node* child = node->data;
        vfs_node* vnode = child->data;

        log_debug("%-*s|- %s ", (int)depth, "", vnode->name);

        _recursive_dump(child, depth + 1);
    }
}

void dump_vfs() {
    assert(vfs);

    log_debug("Recursive dump of the virtual file system:");

    _recursive_dump(vfs->tree->root, 0);
}
