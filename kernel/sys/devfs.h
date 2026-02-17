#pragma once

#include <sys/vfs.h>

typedef bool (*devfs_device_init_fn)(vfs_node_t* dev_dir);

bool devfs_register_node(
    vfs_node_t* parent,
    const char* name,
    u32 type,
    mode_t mode,
    vfs_interface_t* interface,
    void* priv
);
vfs_node_t* devfs_register_dir(vfs_node_t* parent, const char* name, mode_t mode);

bool devfs_register_device(const char* name, devfs_device_init_fn init_fn);

void devfs_init(void);
