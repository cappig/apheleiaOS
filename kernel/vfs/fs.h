#pragma once

#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>

#define VFS_EOF 0

typedef enum {
    VFS_FILE = 1,
    VFS_DIR = 2,
    VFS_SYMLINK = 3,
    VFS_BLOCKDEV = 4,
    VFS_CHARDEV = 5,
} vfs_node_type;

typedef struct {
    u64 created;
    u64 modified;
    u64 accessed;
} vfs_timestamp;

// Resolve the circular references and make the compiler shut up
typedef struct vfs_node vfs_node;

// FIXME: This is a circular header dependency
// Forward declaration to "fix", driver.h must be included by user
typedef struct vfs_driver vfs_driver;

typedef isize (*vfs_read_fn)(vfs_node* node, void* buf, usize offset, usize len);
typedef isize (*vfs_write_fn)(vfs_node* node, void* buf, usize offset, usize len);

typedef struct {
    vfs_read_fn read;
    vfs_write_fn write;
} vfs_node_interface;

typedef struct vfs_node {
    char* name;

    u64 size;
    vfs_node_type type;

    u32 permissions;

    u64 inode;

    vfs_timestamp time;

    vfs_node_interface* interface;
    vfs_driver* driver;

    void* private;
} vfs_node;

typedef struct {
    linked_list* mounted;
    tree* tree;
} virtual_fs;

extern virtual_fs* vfs;


virtual_fs* vfs_init(void);

vfs_node* vfs_create_node(char* name, vfs_node_type type);
void vfs_destroy_node(vfs_node* node);

vfs_node_interface* vfs_create_file_interface(vfs_read_fn read, vfs_write_fn write);
void vfs_destroy_interface(vfs_node_interface* interface);

tree_node* vfs_mount(const char* path, tree_node* mount_node);

tree_node* vfs_lookup_tree_from(tree_node* from, const char* path);
tree_node* vfs_lookup_tree(const char* path);
vfs_node* vfs_lookup(const char* path);

void dump_vfs(void);
