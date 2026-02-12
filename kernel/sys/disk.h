#pragma once

#include <base/types.h>
#include <data/tree.h>
#include <data/vector.h>

#include "sys/types.h"


// files and directories created by the kernel have these permissions
// rw-rw----
#define KFILE_MODE 0660
// rwxr-xr-x
#define KDIR_MODE 0755


enum disk_dev_type {
    DISK_VIRTUAL, // ramdisks, etc...
    DISK_HARD,
    DISK_FLOPPY,
    DISK_OPTICAL,
};

typedef struct fs fs_t;
typedef struct fs_interface fs_interface_t;
typedef struct fs_instance fs_instance_t;
typedef struct vfs_node vfs_node_t;

typedef struct disk_dev disk_dev_t;
typedef struct disk_interface disk_interface_t;
typedef struct disk_partition disk_partition_t;


struct disk_partition {
    char* name;
    size_t type;
    u8 status;
    size_t size;
    size_t offset;

    disk_dev_t* disk;
    fs_instance_t* fs_instance;
};

struct disk_interface {
    ssize_t (*read)(disk_dev_t* dev, void* dest, size_t offset, size_t bytes);
    ssize_t (*write)(disk_dev_t* dev, void* src, size_t offset, size_t bytes);
};

struct disk_dev {
    char* name;
    size_t id;

    size_t type;

    disk_interface_t* interface;

    size_t sector_size;
    size_t sector_count;

    vector_t* partitions;

    void* private;
};


struct fs_interface {
    fs_instance_t* (*probe)(disk_partition_t* partition);

    bool (*build_tree)(fs_instance_t* instance);
    bool (*destroy_tree)(fs_instance_t* instance);

    bool (*chmod)(fs_instance_t* instance, vfs_node_t* node, mode_t mode);
    bool (*chown)(fs_instance_t* instance, vfs_node_t* node, uid_t uid, gid_t gid);

    // mkdir
    // touch
};

struct fs {
    char* name;
    size_t id;

    fs_interface_t* fs_interface;
    fs_interface_t* node_interface;

    void* private;
};

struct fs_instance {
    fs_t* filesystem;
    disk_partition_t* partition;

    bool has_tree;
    tree_node_t* subtree_root;

    size_t refcount;

    void* private;
};

bool disk_register(disk_dev_t* dev);
disk_dev_t* disk_lookup(size_t dev_id);

bool file_system_register(fs_t* fs);
fs_t* file_system_lookup(const char* name);

bool mount_rootfs(disk_dev_t* dev);
bool disk_publish_devices(void);

void dump_partitions(disk_dev_t* dev);
