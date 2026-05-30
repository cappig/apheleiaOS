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

#define VFS_EOF              (-1)
#define VFS_INTERFACE_STATIC ((u32) - 1)

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
    // flags 0...16 are reserved for the OS
    // reads and writes should not append the process to a list of waiters
    VFS_NONBLOCK = 1 << 0,
    // flags 16...32 may be used for device specific flags
};

typedef struct vfs vfs_t;
typedef struct vfs_node vfs_node_t;
typedef struct vfs_interface vfs_interface_t;
struct sched_wait_queue;

typedef ssize_t (*vfs_io_fn)(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags);
typedef ssize_t (*vfs_truncate_fn)(vfs_node_t *node, size_t len);
typedef short (*vfs_poll_fn)(vfs_node_t *node, short events, u32 flags);
typedef struct sched_wait_queue *(*vfs_wait_queue_fn)(vfs_node_t *node, short events, u32 flags);
typedef ssize_t (*vfs_ioctl_fn)(vfs_node_t *node, u64 request, void *args);
typedef ssize_t (*vfs_create_fn)(vfs_node_t *node, vfs_node_t *child);
typedef ssize_t (*vfs_link_fn)(vfs_node_t *node, vfs_node_t *child, vfs_node_t *target);
typedef ssize_t (*vfs_remove_fn)(vfs_node_t *node, vfs_node_t *child);
typedef ssize_t (*vfs_rename_fn)(
    vfs_node_t *old_parent,
    vfs_node_t *child,
    vfs_node_t *new_parent,
    vfs_node_t *target,
    const char *new_name
);

struct vfs_interface {
    u32 refcount;

    // operations on the node itself
    vfs_io_fn read;
    vfs_io_fn write;
    vfs_truncate_fn truncate;
    vfs_poll_fn poll;
    vfs_wait_queue_fn wait_queue;
    vfs_io_fn mmap;
    vfs_ioctl_fn ioctl;

    // operations on children
    vfs_create_fn create;
    vfs_link_fn link;
    vfs_remove_fn remove;
    vfs_rename_fn rename;
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
    u32 nlink;

    struct vfs_node *link; // mount target
    char *symlink_target;

    vfs_interface_t *interface;
    fs_instance_t *fs;

    tree_node_t *tree_entry;
    hashmap_str_t *children_index;
    volatile u32 refs; // internal VFS holds
    volatile u32 open_refs; // live file descriptors
    bool busy; // create/remove is in progress; path lookup should skip it
    bool removed; // unlinked from the tree, but still held by open files

    void *private;
};

vfs_t *vfs_init(void);

vfs_node_t *vfs_create_node(char *name, u32 type);
void vfs_destroy_node(vfs_node_t *node);
void vfs_node_retain(vfs_node_t *node);
void vfs_node_release(vfs_node_t *node);
void vfs_node_open(vfs_node_t *node);
void vfs_node_close(vfs_node_t *node);
void vfs_set_interface(vfs_node_t *node, vfs_interface_t *interface);
void vfs_adopt_interface(vfs_node_t *node, vfs_interface_t *interface);
void vfs_clear_interface(vfs_node_t *node);
void vfs_make_virtual(vfs_node_t *node);

vfs_interface_t *vfs_create_interface(vfs_io_fn read, vfs_io_fn write, vfs_truncate_fn truncate);
void vfs_destroy_interface(vfs_interface_t *interface);

bool vfs_validate_name(const char *name);

vfs_node_t *vfs_lookup_from(vfs_node_t *from, const char *path);
vfs_node_t *vfs_lookup(const char *path);
vfs_node_t *vfs_lookup_relative(const char *root, const char *path);
vfs_node_t *vfs_open(const char *path, u32 type, bool create, mode_t mode);
vfs_node_t *vfs_resolve_node(vfs_node_t *node);

int vfs_access(vfs_node_t *vnode, uid_t uid, gid_t gid, int mode);
int vfs_check_search(const char *path, uid_t uid, gid_t gid, bool allow_missing_leaf);
int vfs_stat_node(vfs_node_t *node, stat_t *out, bool follow_links);
int vfs_chmod(vfs_node_t *node, mode_t mode);
int vfs_chown(vfs_node_t *node, uid_t uid, gid_t gid);
int vfs_hardlink(const char *target, const char *link_path);
int vfs_symlink(const char *target, const char *link_path);
MUST_USE int vfs_unlink(const char *path);
MUST_USE int vfs_rmdir(const char *path);
int vfs_detach_child(vfs_node_t *parent, vfs_node_t *child);
int vfs_rename(const char *old_path, const char *new_path);

int vfs_insert_child(vfs_node_t *parent, vfs_node_t *child);
int vfs_insert_child_virtual(vfs_node_t *parent, vfs_node_t *child);
vfs_node_t *vfs_create(vfs_node_t *parent, char *name, u32 type, mode_t mode);
vfs_node_t *vfs_create_virtual(vfs_node_t *parent, char *name, u32 type, mode_t mode);

int vfs_mount(fs_instance_t *fs, vfs_node_t *mount);
int vfs_unmount(vfs_node_t *mount, bool destroy_tree);

ssize_t vfs_read(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags);
ssize_t vfs_write(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags);
ssize_t vfs_truncate(vfs_node_t *node, size_t len);
ssize_t vfs_mmap(vfs_node_t *node, void *buf, size_t offset, size_t len, size_t flags);
ssize_t vfs_ioctl(vfs_node_t *node, u64 request, void *args);
short vfs_poll(vfs_node_t *node, short events, size_t flags);
struct sched_wait_queue *vfs_wait_queue(vfs_node_t *node, short events, size_t flags);

void dump_vfs(void);
