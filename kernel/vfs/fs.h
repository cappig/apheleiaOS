#pragma once

#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>

#include "sys/disk.h"

#define VFS_EOF (-1)

#define VFS_INVALID_TYPE ((vfs_node_type)(-1))

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

typedef struct vfs_node vfs_node;
typedef struct file_system_instance file_system_instance;

typedef isize (*vfs_read_fn)(vfs_node* node, void* buf, usize offset, usize len);
typedef isize (*vfs_write_fn)(vfs_node* node, void* buf, usize offset, usize len);

typedef struct vfs_node_interface {
    vfs_read_fn read;
    vfs_write_fn write;
} vfs_node_interface;

typedef struct vfs_node {
    char* name;

    vfs_timestamp time;

    u64 size;
    vfs_node_type type;

    u64 inode;

    u16 permissions;

    tree_node* tree_entry;

    vfs_node_interface* interface;
    file_system_instance* fs;

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

vfs_node_interface* vfs_create_interface(vfs_read_fn read, vfs_write_fn write);
void vfs_destroy_interface(vfs_node_interface* interface);

bool vfs_validate_name(const char* name);

vfs_node* vfs_lookup_from(vfs_node* from, const char* path);
vfs_node* vfs_lookup(const char* path);
vfs_node* vfs_lookup_relative(const char* root, const char* path);

bool vfs_insert_child(vfs_node* parent, vfs_node* child);

void dump_vfs(void);
