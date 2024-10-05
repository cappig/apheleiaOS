#include "driver.h"

#include <base/types.h>
#include <boot/mbr.h>
#include <data/tree.h>
#include <log/log.h>
#include <stdio.h>
#include <string.h>

#include "drivers/iso9660.h"
#include "drivers/mbr.h"
#include "fs.h"
#include "mem/heap.h"


static usize _get_next_id(void) {
    static usize id = 0;
    return id++;
}

static char* _get_partition_name(char* drive, usize number) {
    // Up to two hex chars and a null terminator
    usize name_len = strlen(drive) + 1 + 2 + 1;
    char* name = kcalloc(name_len);

    sprintf(name, "%sp%zx", drive, number);

    return name;
}

static void _destroy_fs(vfs_file_system* fs, vfs_driver* dev) {
    log_debug("No valid file system found on drive %s", dev->name);
    kfree(fs->partition.name);
    kfree(fs);
}

static bool _probe_iso(virtual_fs* vfs, vfs_driver* dev, vfs_file_system* fs) {
    if (iso_init(dev, fs)) {
        log_info("Detected a valid ISO-9660 file system on drive %s", dev->name);
        vfs_mount(vfs, "/mnt", fs->subtree->root);

        return true;
    } else {
        _destroy_fs(fs, dev);

        return false;
    }
}


static bool _init_optical(virtual_fs* vfs, vfs_driver* dev) {
    vfs_file_system* fs = kcalloc(sizeof(vfs_file_system));

    fs->partition.name = _get_partition_name(dev->name, 1);
    fs->partition.size = dev->disk_size;
    fs->partition.offset = 0;

    return _probe_iso(vfs, dev, fs);
}

static bool _init_hard(virtual_fs* vfs, vfs_driver* dev) {
    mbr_table* table = parse_mbr(dev);

    if (!table) {
        log_debug("No valid file system found on drive %s", dev->name);
        return false;
    }

    log_debug("Partitions found on drive %s:", dev->name);
    dump_mbr(table);

    // We kind of have to trust the partition table at this point
    for (usize i = 0; i < 4; i++) {
        mbr_partition* part = &table->partitions[i];

        if (!part->type)
            continue;

        vfs_file_system* fs = kcalloc(sizeof(vfs_file_system));

        fs->partition.name = _get_partition_name(dev->name, i);
        fs->partition.type = part->type;
        fs->partition.size = part->sector_count * dev->sector_size;
        fs->partition.offset = part->lba_first * dev->sector_size;

        switch (part->type) {
        case MBR_ISO:
            return _probe_iso(vfs, dev, fs);
        default:
            _destroy_fs(fs, dev);
            return false;
        }
    }

    return false;
}

static bool _probe_fs(virtual_fs* vfs, vfs_driver* dev) {
    switch (dev->type) {
    case VFS_DRIVER_HARD:
        return _init_hard(vfs, dev);
    case VFS_DRIVER_OPTICAL:
        return _init_optical(vfs, dev);
    default:
        log_error("Disk has unknown driver type!");
        return false;
    }
}


vfs_driver* vfs_create_device(const char* name, usize sector_size, usize sector_count) {
    vfs_driver* ret = kcalloc(sizeof(vfs_driver));

    ret->name = strdup(name);
    ret->id = _get_next_id();

    ret->sector_size = sector_size;
    ret->disk_size = sector_count * sector_size;

    return ret;
}

void vfs_destroy_device(vfs_driver* dev) {
    kfree(dev->interface);
    kfree(dev->name);
    kfree(dev);
}

// Mount a new device to the tree and probe for a valid file system
tree_node* vfs_regiter(virtual_fs* vfs, const char* path, vfs_driver* dev) {
    vfs_node* mount_node = vfs_create_node(dev->name, VFS_BLOCKDEV);
    tree_node* mount_point = vfs_mount(vfs, path, tree_create_node(mount_node));

    log_info("Mounted %s/%s", path, dev->name);

    _probe_fs(vfs, dev);

    return mount_point;
}


vfs_file_system* vfs_crete_fs(char* name) {
    vfs_file_system* ret = kcalloc(sizeof(vfs_file_system));

    vfs_node* mount = vfs_create_node(name, VFS_MOUNT);

    ret->subtree = tree_create(mount);

    // vfs_node_interface* interface = kcalloc(sizeof(vfs_node_interface));
    // interface->mount.subtree = ret->subtree;

    return ret;
}

void vfs_destroy_fs(vfs_file_system* fs) {
    tree_destory(fs->subtree);
}
