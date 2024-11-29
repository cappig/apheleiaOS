#pragma once

#include "fs.h"

// A vfs_driver represents an underlying disk device, that is it abstracts the
// underlying logic and exposes a simple interface that allows us to read
// and write fixed size blocks of data

typedef enum {
    VFS_DRIVER_VIRTUAL,
    VFS_DRIVER_HARD,
    VFS_DRIVER_OPTICAL,
} vfs_driver_type;

typedef struct vfs_driver vfs_driver;

// NOTE: a device might be read only. In that case write is set to NULL
typedef struct {
    isize (*read)(vfs_driver* dev, void* dest, usize offset, usize bytes);
    isize (*write)(vfs_driver* dev, void* src, usize offset, usize bytes);
} vfs_drive_interface;

typedef struct vfs_driver {
    char* name;
    usize id;

    vfs_driver_type type;

    vfs_drive_interface* interface;

    usize sector_size;
    usize disk_size;

    void* private;
} vfs_driver;


// A vfs_file_system represents an actual file system on a disk. It abstracts
// away fs specific logic and allows the vfs to provide a uniform API for file access
// A single vfs_driver might have more than one vfs_file_system, each one
// lives on a separate partition on said drive.
// A vfs_file_system is responsible for maintaining a subtree, that is a branch
// of the vfs tree rooted at the fs mount point node

typedef struct {
    char* name;
    usize type;
    usize size;
    usize offset;
} disk_partition;

typedef struct vfs_file_system {
    disk_partition partition;

    vfs_node_interface* interface;

    tree* subtree;

    void* private;
} vfs_file_system;


vfs_driver* vfs_create_device(const char* name, usize sector_size, usize sector_count);
void vfs_destroy_device(vfs_driver* dev);

vfs_file_system* vfs_crete_fs(char* name);
void vfs_destroy_fs(vfs_file_system* fs);

tree_node* vfs_register(virtual_fs* vfs, const char* path, vfs_driver* dev);
