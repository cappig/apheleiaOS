#include "fs.h"

#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>
#include <errno.h>
#include <log/log.h>
#include <string.h>

#include "mem/heap.h"


virtual_fs* vfs_init() {
    virtual_fs* vfs = kcalloc(sizeof(virtual_fs));

    vfs->mounted = list_create();

    vfs_node* root = vfs_create_node(NULL, VFS_DIR);
    vfs->tree = tree_create(root);

    vfs_node* dev = vfs_create_node("dev", VFS_DIR);
    vfs_node* mnt = vfs_create_node("mnt", VFS_DIR);

    vfs_mount(vfs, "/", tree_create_node(dev));
    vfs_mount(vfs, "/", tree_create_node(mnt));

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


tree_node* vfs_lookup_tree(virtual_fs* vfs, const char* path) {
    if (!path || path[0] != '/') {
        errno = EINVAL;
        return NULL;
    }

    tree_node* node = vfs->tree->root;
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
            errno = ENOENT;
            return NULL;
        }

        pos = strtok_r(NULL, "/", &tok_pos);
    }

    kfree(tok_str);
    return node;
}

vfs_node* vfs_lookup(virtual_fs* vfs, const char* path) {
    tree_node* tnode = vfs_lookup_tree(vfs, path);

    if (tnode)
        return tnode->data;
    else
        return NULL;
}


tree_node* vfs_mount(virtual_fs* vfs, const char* path, tree_node* mount_node) {
    vfs_node* node = mount_node->data;
    if (!node->name) {
        errno = EINVAL;
        return NULL;
    }

    tree_node* parent_node = vfs_lookup_tree(vfs, path);
    if (!parent_node) {
        errno = ENOENT;
        return NULL;
    }

    // TODO: This should be a hash map
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

void dump_vfs(virtual_fs* vfs) {
    log_debug("Recursive dump of viral file system:");

    _recursive_dump(vfs->tree->root, 0);
}
