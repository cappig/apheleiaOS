#pragma once

#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>
#include <sys/types.h>
#include <time.h>

#include "disk.h"

#define VFS_EOF (-1)

// regular and device files should provide the read/(write) interface
#define VFS_IS_READABLE(type) ((type) >= VFS_FILE && (type) <= VFS_CHARDEV)
#define VFS_IS_LINK(type)     ((type) == VFS_SYMLINK || (type) == VFS_MOUNT)
#define VFS_IS_DEVICE(type)   ((type) == VFS_BLOCKDEV || (type) == VFS_CHARDEV)

enum vfs_node_type {
    VFS_INVALID = 0,
    VFS_FILE = 1,
    VFS_BLOCKDEV = 2,
    VFS_CHARDEV = 3,
    VFS_DIR = 4,
    VFS_SYMLINK = 5,
    VFS_MOUNT = 6,
};

typedef struct {
    time_t created;
    time_t modified;
    time_t accessed;
} vfs_time_t;

enum vfs_flags {
    // Flags 0...16 are reserved for the OS
    // Reads and writes should not append the process to a list of waiters
    VFS_NONBLOCK = 1 << 0,
    // Flags 16...32 may be used for device specific flags
};


typedef struct vfs vfs_t;
typedef struct vfs_node vfs_node_t;
typedef struct vfs_interface vfs_interface_t;

// typedef ssize_t (*vfs_read_fn)(vfs_node_t* node, void* buf, size_t offset, size_t len, u32
// flags); typedef ssize_t (*vfs_write_fn)(vfs_node_t* node, void* buf, size_t offset, size_t len,
// u32 flags); typedef ssize_t (*vfs_mmap_fn)(vfs_node_t* node, void* buf, size_t offset, size_t
// len, u32 flags); typedef ssize_t (*vfs_ioctl_fn)(vfs_node_t* node, u64 request, void* args);
// typedef ssize_t (*vfs_create_fn)(vfs_node_t* node, vfs_node_t* child);
// typedef ssize_t (*vfs_remove_fn)(vfs_node_t* node, char* name);

struct vfs_interface {
    u32 refcount;

    // Operations on the node itself
    ssize_t (*read)(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags);
    ssize_t (*write)(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags);

    ssize_t (*mmap)(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags);
    ssize_t (*ioctl)(vfs_node_t* node, u64 request, void* args);

    // Operations on children
    ssize_t (*create)(vfs_node_t* node, vfs_node_t* child);
    ssize_t (*remove)(vfs_node_t* node, char* name);
};

struct vfs_node {
    char* name;
    u32 type;

    vfs_time_t time;

    mode_t mode;
    uid_t uid;
    gid_t gid;

    u64 size;
    u64 inode;

    struct vfs_node* link; // The target if this node is a symlink or a mount point

    vfs_interface_t* interface;
    fs_instance_t* fs;

    tree_node_t* tree_entry;

    void* private;
};

struct vfs {
    tree_t* tree;
};
