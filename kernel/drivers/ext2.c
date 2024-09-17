#include "ext2.h"

#include <base/macros.h>

#include "mem/heap.h"
#include "vfs/driver.h"


static ext2_device_private* _parse_superblock(vfs_driver* dev, vfs_file_system* fs) {
    ext2_superblock* super = kmalloc(sizeof(ext2_superblock));

    usize loc = fs->partition.offset + 1024;
    dev->interface->read(dev, super, loc, sizeof(ext2_superblock));

    if (super->signature != EXT2_SIGNATURE) {
        kfree(super);
        return NULL;
    }

    ext2_device_private* priv = kcalloc(sizeof(ext2_device_private));

    priv->superblock = super;

    priv->block_size = 1024 << super->block_size_shift;
    priv->block_count = super->block_count;

    priv->indirect_block_size = priv->block_size / sizeof(u32);

    return priv;
}

static void _read_descriptor_table(vfs_driver* dev, vfs_file_system* fs) {
    ext2_device_private* priv = fs->private;
    ext2_superblock* superblock = priv->superblock;

    priv->group_table_count = DIV_ROUND_UP(superblock->block_count, superblock->blocks_in_group);

    usize table_size = priv->group_table_count * sizeof(ext2_group_descriptor);
    usize table_location = fs->partition.offset + 1024 + sizeof(ext2_superblock);

    priv->group_table = kmalloc(table_size);

    dev->interface->read(dev, priv->group_table, table_location, table_size);
}


bool ext2_init(vfs_driver* dev, vfs_file_system* fs) {
    // TODO: finish ext2 support
    return false;

    ext2_device_private* priv = _parse_superblock(dev, fs);

    if (!priv)
        return false;

    _read_descriptor_table(dev, fs);

    return true;
}
