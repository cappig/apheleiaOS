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

    vfs_node* root = vfs_create_node("/", VFS_DIR);

    vfs->tree.root = tree_create_node(root);
    vfs->tree.nodes = 1;

    vfs_mount(vfs, "/", vfs_create_node("dev", VFS_DIR));
    vfs_mount(vfs, "/", vfs_create_node("mnt", VFS_DIR));

    return vfs;
}

vfs_node* vfs_create_node(char* name, vfs_node_type type) {
    vfs_node* new = kcalloc(sizeof(vfs_node));
    new->name = strdup(name);
    new->type = type;

    return new;
}

tree_node* vfs_lookup(virtual_fs* vfs, const char* path) {
    if (!path || path[0] != '/') {
        errno = EINVAL;
        return NULL;
    }

    tree_node* node = vfs->tree.root;
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

tree_node* vfs_mount(virtual_fs* vfs, const char* path, vfs_node* node) {
    tree_node* parent_node = vfs_lookup(vfs, path);
    if (!parent_node)
        return NULL;

    tree_node* mount_node = tree_create_node(node);
    tree_insert_child(parent_node, mount_node);
    // TODO: add time stuff

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
    log_debug("Recursive dump of virual file system:");

    log_debug("/");
    _recursive_dump(vfs->tree.root, 0);
}
