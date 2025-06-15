#pragma once

#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>

#include "sys/disk.h"

#define VFS_EOF (-1)

// regular and device files should provide the read/(write) interface
#define VFS_IS_READABLE(type) ((type) >= VFS_FILE && (type) <= VFS_CHARDEV)
#define VFS_IS_LINK(type)     ((type) == VFS_SYMLINK || (type) == VFS_MOUNT)
#define VFS_IS_DEVICE(type)   ((type) == VFS_BLOCKDEV || (type) == VFS_CHARDEV)

typedef enum {
    VFS_INVALID = 0,

    VFS_FILE = 1,
    VFS_BLOCKDEV = 2,
    VFS_CHARDEV = 3,
    VFS_DIR = 4,
    VFS_SYMLINK = 5,
    VFS_MOUNT = 6,
} vfs_node_type;

typedef struct {
    u64 created;
    u64 modified;
    u64 accessed;
} vfs_timestamp;

enum vfs_flags {
    // Flags 0...16 are reserved for the OS
    // Reads and writes should not append the process to a list of waiters
    VFS_NONBLOCK = 1 << 0,

    // Flags 16...32 may be used for device specific flags
};

typedef u16 vfs_mode;

typedef struct vfs_node vfs_node;
typedef struct file_system_instance file_system_instance;

typedef isize (*vfs_read_fn)(vfs_node* node, void* buf, usize offset, usize len, u32 flags);
typedef isize (*vfs_write_fn)(vfs_node* node, void* buf, usize offset, usize len, u32 flags);
typedef isize (*vfs_mmap_fn)(vfs_node* node, void* buf, usize offset, usize len, u32 flags);
typedef isize (*vfs_ioctl_fn)(vfs_node* node, u64 request, void* args);
typedef isize (*vfs_create_fn)(vfs_node* node, vfs_node* child);
typedef isize (*vfs_remove_fn)(vfs_node* node, char* name);

typedef struct vfs_node_interface {
    // Operations on the node itself
    vfs_read_fn read;
    vfs_write_fn write;

    vfs_mmap_fn mmap; // only for devices
    vfs_ioctl_fn ioctl; // only for char devices

    // Operations on children
    vfs_create_fn create;
    vfs_remove_fn remove;
} vfs_node_interface;

typedef struct vfs_node {
    char* name;
    vfs_node_type type;

    vfs_timestamp time;

    vfs_mode permissions;
    usize uid;
    usize gid;

    u64 size;
    u64 inode;

    vfs_node* link; // The target if this node is a symlink or a mount point

    vfs_node_interface* interface;
    file_system_instance* fs;

    tree_node* tree_entry;

    void* private;
} vfs_node;

typedef struct {
    tree* tree;
} virtual_fs;


virtual_fs* vfs_init(void);

vfs_node* vfs_create_node(char* name, vfs_node_type type);
void vfs_destroy_node(vfs_node* node);

vfs_node_interface* vfs_create_interface(vfs_read_fn read, vfs_write_fn write);
void vfs_destroy_interface(vfs_node_interface* interface);

bool vfs_validate_name(const char* name);

vfs_node* vfs_lookup_from(vfs_node* from, const char* path);
vfs_node* vfs_lookup(const char* path);
vfs_node* vfs_lookup_relative(const char* root, const char* path);
vfs_node* vfs_open(const char* path, vfs_node_type type, bool create, vfs_mode mode);

bool vfs_insert_child(vfs_node* parent, vfs_node* child);
vfs_node* vfs_create(vfs_node* parent, char* name, vfs_node_type type, vfs_mode mode);

bool vfs_mount(file_system_instance* fs, vfs_node* mount);
bool vfs_unmount(vfs_node* mount, bool destroy_tree);

isize vfs_read(vfs_node* node, void* buf, usize offset, usize len, usize flags);
isize vfs_write(vfs_node* node, void* buf, usize offset, usize len, usize flags);
isize vfs_mmap(vfs_node* node, void* buf, usize offset, usize len, usize flags);

void dump_vfs(void);
