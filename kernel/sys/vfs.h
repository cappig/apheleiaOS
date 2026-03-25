#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <data/hashmap.h>
#include <data/list.h>
#include <data/tree.h>
#include <poll.h>
#include <sys/stat.h>
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
struct sched_wait_queue;

struct vfs_interface {
    u32 refcount;

    // Operations on the node itself
    ssize_t (*read)(
        vfs_node_t *node,
        void *buf,
        size_t offset,
        size_t len,
        u32 flags
    );
    ssize_t (*write)(
        vfs_node_t *node,
        void *buf,
        size_t offset,
        size_t len,
        u32 flags
    );
    ssize_t (*truncate)(vfs_node_t *node, size_t len);
    short (*poll)(vfs_node_t *node, short events, u32 flags);
    struct sched_wait_queue *(*wait_queue)(
        vfs_node_t *node,
        short events,
        u32 flags
    );

    ssize_t (*mmap)(
        vfs_node_t *node,
        void *buf,
        size_t offset,
        size_t len,
        u32 flags
    );
    ssize_t (*ioctl)(vfs_node_t *node, u64 request, void *args);

    // Operations on children
    ssize_t (*create)(vfs_node_t *node, vfs_node_t *child);
    ssize_t (*remove)(vfs_node_t *node, char *name);
};

struct vfs_node {
    char *name;
    u32 type;

    vfs_time_t time;

    mode_t mode;
    uid_t uid;
    gid_t gid;

    u64 size;
    u64 inode;

    struct vfs_node *link; // The target if this node is a symlink or a mount point
    char *symlink_target;

    vfs_interface_t *interface;
    fs_instance_t *fs;

    tree_node_t *tree_entry;
    hashmap_str_t *children_index;
    volatile u32 open_refs;

    void *private;
};

struct vfs {
    tree_t *tree;
};


vfs_t *vfs_init(void);

vfs_node_t *vfs_create_node(char *name, u32 type);
void vfs_destroy_node(vfs_node_t *node);

vfs_interface_t *vfs_create_interface(
    ssize_t (*read)(
        vfs_node_t *node,
        void *buf,
        size_t offset,
        size_t len,
        u32 flags
    ),
    ssize_t (*write)(
        vfs_node_t *node,
        void *buf,
        size_t offset,
        size_t len,
        u32 flags
    ),
    ssize_t (*truncate)(vfs_node_t *node, size_t len)
);
void vfs_destroy_interface(vfs_interface_t *interface);

bool vfs_validate_name(const char *name);

vfs_node_t *vfs_lookup_from(vfs_node_t *from, const char *path);
vfs_node_t *vfs_lookup(const char *path);
vfs_node_t *vfs_lookup_relative(const char *root, const char *path);
vfs_node_t *vfs_open(const char *path, u32 type, bool create, mode_t mode);

bool vfs_access(vfs_node_t *vnode, uid_t uid, gid_t gid, int mode);
int vfs_check_search(
    const char *path,
    uid_t uid,
    gid_t gid,
    bool allow_missing_leaf
);
bool vfs_stat_node(vfs_node_t *node, stat_t *out, bool follow_links);
bool vfs_chmod(vfs_node_t *node, mode_t mode);
bool vfs_chown(vfs_node_t *node, uid_t uid, gid_t gid);
bool vfs_link(const char *target, const char *link_path);
MUST_USE bool vfs_unlink(const char *path);
MUST_USE bool vfs_rmdir(const char *path);
bool vfs_detach_child(vfs_node_t *parent, vfs_node_t *child);
bool vfs_rename(const char *old_path, const char *new_path);

bool vfs_insert_child(vfs_node_t *parent, vfs_node_t *child);
bool vfs_insert_child_virtual(vfs_node_t *parent, vfs_node_t *child);
vfs_node_t *vfs_create(vfs_node_t *parent, char *name, u32 type, mode_t mode);
vfs_node_t *vfs_create_virtual(vfs_node_t *parent, char *name, u32 type, mode_t mode);

bool vfs_mount(fs_instance_t *fs, vfs_node_t *mount);
bool vfs_unmount(vfs_node_t *mount, bool destroy_tree);

ssize_t
vfs_read(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags);
ssize_t
vfs_write(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags);
ssize_t vfs_truncate(vfs_node_t *node, size_t len);
ssize_t
vfs_mmap(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags);
ssize_t vfs_ioctl(vfs_node_t *node, u64 request, void *args);
short vfs_poll(vfs_node_t *node, short events, size_t flags);
struct sched_wait_queue *
vfs_wait_queue(vfs_node_t *node, short events, size_t flags);

void dump_vfs(void);
