#include "ext2.h"

#include <base/addr.h>
#include <base/macros.h>
#include <x86/paging.h>

#include "mem/heap.h"
#include "mem/physical.h"
#include "sys/disk.h"

static file_system fs = {.name = "ext2"};


static ext2_device_private* _parse_superblock(disk_partition* part) {
    usize pages = DIV_ROUND_UP(sizeof(ext2_superblock), PAGE_4KIB);

    void* paddr = alloc_frames(pages);
    ext2_superblock* super = (void*)ID_MAPPED_VADDR(paddr);

    disk_dev* dev = part->disk;

    usize loc = part->offset + 1024;
    dev->interface->read(dev, super, loc, sizeof(ext2_superblock));

    if (super->signature != EXT2_SIGNATURE) {
        free_frames(super, pages);
        return NULL;
    }

    ext2_device_private* priv = kcalloc(sizeof(ext2_device_private));

    priv->superblock = super;

    priv->block_size = 1024 << super->block_size_shift;
    priv->block_count = super->block_count;

    priv->indirect_block_size = priv->block_size / sizeof(u32);

    return priv;
}

static void _read_descriptor_table(file_system_instance* instance) {
    ext2_device_private* priv = instance->private;
    disk_dev* dev = instance->partition->disk;
    ext2_superblock* superblock = priv->superblock;

    priv->group_table_count = DIV_ROUND_UP(superblock->block_count, superblock->blocks_in_group);

    usize table_size = priv->group_table_count * sizeof(ext2_group_descriptor);
    usize table_location = instance->partition->offset + 1024 + sizeof(ext2_superblock);

    priv->group_table = kmalloc(table_size);

    dev->interface->read(dev, priv->group_table, table_location, table_size);
}


static file_system_instance* _probe(disk_partition* part) {
    ext2_device_private* priv = _parse_superblock(part);

    if (!priv)
        return NULL;

    file_system_instance* instance = kcalloc(sizeof(file_system_instance));

    instance->fs = &fs;
    instance->private = priv;

    return instance;
}

static bool _build_tree(file_system_instance* instance) {
    if (instance->fs->id != fs.id)
        return false;

    // ext2_device_private* priv = instance->private;
    // instance->tree_build = true;

    return false;
}


bool ext2_init() {
    // TODO: finish ext2 support
    return false;

    file_system_interface* fs_interface = kcalloc(sizeof(file_system_interface));

    fs_interface->probe = _probe;

    fs.fs_interface = fs_interface;

    vfs_node_interface* node_interface = kcalloc(sizeof(vfs_node_interface));

    node_interface->read = NULL;
    node_interface->write = NULL;


    file_system_register(&fs);

    return true;
}
