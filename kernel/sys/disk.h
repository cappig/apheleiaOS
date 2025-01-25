#pragma once

#include <base/types.h>
#include <data/vector.h>

#include "vfs/fs.h"


typedef struct vfs_node_interface vfs_node_interface;

enum disk_dev_type {
    DISK_VIRTUAL, // ramdisks, etc...
    DISK_HARD,
    DISK_FLOPPY,
    DISK_OPTICAL,
};

typedef struct disk_dev disk_dev;

typedef struct {
    char* name;
    usize type;
    usize size;
    usize offset;

    disk_dev* disk;
} disk_partition;

typedef struct {
    isize (*read)(disk_dev* dev, void* dest, usize offset, usize bytes);
    isize (*write)(disk_dev* dev, void* src, usize offset, usize bytes);
} disk_dev_interface;

typedef struct disk_dev {
    char* name;
    usize id;

    usize type;

    disk_dev_interface* interface;

    usize sector_size;
    usize sector_count;

    vector* partitions;

    void* private;
} disk_dev;


typedef struct file_system file_system;
typedef struct file_system_instance file_system_instance;

typedef struct vfs_node vfs_node;

typedef struct {
    file_system_instance* (*probe)(disk_partition* part);

    bool (*mount)(file_system_instance* instance, vfs_node* mount);
    bool (*unmount)(file_system_instance* instance);

    // mkdir
    // touch
} file_system_interface;

typedef struct file_system {
    char* name;
    usize id;

    file_system_interface* fs_interface;
    vfs_node_interface* node_interface;

    void* private;
} file_system;


typedef struct file_system_instance {
    file_system* fs;

    disk_partition* partition;

    bool tree_build;
    vfs_node* mount;

    void* private;
} file_system_instance;


bool file_system_register(file_system* fs);

bool disk_register(disk_dev* dev);

void dump_partitions(disk_dev* dev);