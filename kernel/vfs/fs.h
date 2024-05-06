#pragma once

#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>

typedef enum : u32 {
    VFS_FILE = 1,
    VFS_DIR = 2,
    VFS_BLOCKDEV = 3,
    VFS_CHARDEV = 4,
    // TODO: etc . . .
} vfs_node_type;

typedef struct {
    u64 created;
    u64 modified;
    u64 accessed;
} vfs_timestamp;

typedef u32 vfs_mode;

// Resolve the circular referances and make the compiler shut up
typedef struct vfs_node vfs_node;

typedef isize (*vfs_read_fn)(vfs_node* node, void* buf, usize offset, usize len);
typedef isize (*vfs_write_fn)(vfs_node* node, void* buf, usize offset, usize len);
typedef isize (*vfs_touch_fn)(vfs_node* parent, char* name, vfs_node permissions);

typedef struct {
    struct {
        vfs_read_fn read;
        vfs_write_fn write;
    } file;
    struct {
        vfs_touch_fn touch;
    } dir;
} vfs_node_interface;

typedef struct vfs_node {
    char* name;
    u64 size;
    vfs_timestamp time;

    vfs_node_type type;

    u32 permissions;
    u32 flags;

    u32 inode;
    u32 device;

    vfs_node_interface* interface;

    void* private;
} vfs_node;

typedef struct {
    linked_list mounted;
    tree tree;
} virtual_fs;


virtual_fs* vfs_init(void);
vfs_node* vfs_create_node(char* name, vfs_node_type type);

tree_node* vfs_lookup(virtual_fs* vfs, const char* path);
tree_node* vfs_mount(virtual_fs* vfs, const char* path, vfs_node* node);

void dump_vfs(virtual_fs* vfs);
