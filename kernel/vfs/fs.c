#include "fs.h"

#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>
#include <errno.h>
#include <log/log.h>
#include <string.h>

#include "mem/heap.h"
#include "sys/panic.h"

// Our kernel has one single virtual file system
virtual_fs* vfs = NULL;


virtual_fs* vfs_init() {
    vfs = kcalloc(sizeof(virtual_fs));
    assert(vfs);

    vfs->mounted = list_create();

    vfs_node* root = vfs_create_node(NULL, VFS_DIR);
    vfs->tree = tree_create(root);

    vfs_node* dev = vfs_create_node("dev", VFS_DIR);
    vfs_node* mnt = vfs_create_node("mnt", VFS_DIR);

    vfs_mount("/", tree_create_node(dev));
    vfs_mount("/", tree_create_node(mnt));

    return vfs;
}


vfs_node* vfs_create_node(char* name, vfs_node_type type) {
    vfs_node* new = kcalloc(sizeof(vfs_node));
    new->type = type;

    if (name)
        new->name = strdup(name);

    return new;
}

void vfs_destroy_node(vfs_node* node) {
    if (node->interface)
        kfree(node->interface);

    if (node->name)
        kfree(node->name);

    kfree(node);
}

vfs_node_interface* vfs_create_file_interface(vfs_read_fn read, vfs_write_fn write) {
    vfs_node_interface* interface = kcalloc(sizeof(vfs_node_interface));
    interface->read = read;
    interface->write = write;

    return interface;
}

void vfs_destroy_interface(vfs_node_interface* interface) {
    kfree(interface);
}


tree_node* vfs_lookup_tree_from(tree_node* from, const char* path) {
    assert(vfs);

    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    tree_node* node = from;
    if (!node) {
        errno = ENXIO;
        return NULL;
    }

    char* tok_pos = NULL;
    char* tok_str = strdup(path);
    char* pos = strtok_r(tok_str, "/", &tok_pos);

    while (pos) {
        bool found = false;

        foreach (child, node->children) {
            vfs_node* child_vfs = ((tree_node*)child->data)->data;

            if (!strcmp(child_vfs->name, pos)) {
                found = true;
                node = child->data;
                break;
            }
        }

        if (!found) {
            kfree(tok_str);

            errno = ENOENT;
            return NULL;
        }

        pos = strtok_r(NULL, "/", &tok_pos);
    }

    kfree(tok_str);
    return node;
}

tree_node* vfs_lookup_tree(const char* path) {
    if (!path || path[0] != '/') {
        errno = EINVAL;
        return NULL;
    }

    return vfs_lookup_tree_from(vfs->tree->root, path);
}

vfs_node* vfs_lookup(const char* path) {
    tree_node* tnode = vfs_lookup_tree(path);

    if (!tnode)
        return NULL;

    return tnode->data;
}

vfs_node* vfs_lookup_from(const char* from, const char* path) {
    tree_node* tnode_from = vfs_lookup_tree(from);

    if (!tnode_from)
        return NULL;

    tree_node* tnode = vfs_lookup_tree_from(tnode_from, path);

    if (!tnode)
        return NULL;

    return tnode->data;
}

tree_node* vfs_mount(const char* path, tree_node* mount_node) {
    assert(vfs);

    vfs_node* node = mount_node->data;
    if (!node->name) {
        errno = EINVAL;
        return NULL;
    }

    tree_node* parent_node = vfs_lookup_tree(path);
    if (!parent_node) {
        errno = ENOENT;
        return NULL;
    }

    // FIXME: VFS file names should be unique inside a given folder
    foreach (child, parent_node->children) {
        tree_node* child_node = child->data;
        vfs_node* child_vnode = child_node->data;

        if (!strcmp(child_vnode->name, node->name)) {
            errno = EEXIST;
            return NULL;
        }
    }

    tree_insert_child(parent_node, mount_node);

    return mount_node;
}


static void _recursive_dump(tree_node* parent, usize depth) {
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
