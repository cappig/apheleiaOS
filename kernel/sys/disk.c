#include "disk.h"

#include <base/types.h>
#include <boot/mbr.h>
#include <data/vector.h>
#include <log/log.h>
#include <stdio.h>
#include <string.h>

#include "drivers/mbr.h"
#include "mem/heap.h"
#include "sys/panic.h"
#include "vfs/fs.h"

static vector* file_systems = NULL;


static usize _get_disk_id(void) {
    static usize id = 1;
    return id++;
}

static usize _get_fs_id(void) {
    static usize id = 1;
    return id++;
}

static char* _partition_name(char* disk, isize number) {
    if (number == -1)
        return strdup(disk);

    usize len = strlen(disk) + 1 + 2; // up to two hex chars
    char* name = kcalloc(len + 1); // and a terminator

    snprintf(name, len, "%sp%zx", disk, number);

    return name;
}

static bool _create_partition(disk_dev* dev, isize number, usize sectors, usize offset) {
    disk_partition* part = kcalloc(sizeof(disk_partition));

    part->name = _partition_name(dev->name, number);
    part->size = dev->sector_size * sectors;
    part->offset = offset;
    part->disk = dev;

    return vec_push(dev->partitions, part);
}

static bool _parse_mbr(disk_dev* dev) {
    mbr_table* mbr = parse_mbr(dev);

    if (!mbr)
        return false;

    if (mbr_is_empty(mbr)) {
        // MBR exists but it's empty. Assume its a protective MBR that maps the whole disk
        _create_partition(dev, -1, dev->sector_count, 0);
    } else {
        for (usize i = 0; i < 4; i++) {
            mbr_partition* part = &mbr->partitions[i];

            if (!part->type)
                continue;

            usize size = part->sector_count * dev->sector_size;
            usize offset = part->lba_first * dev->sector_size;

            _create_partition(dev, i, size, offset);
        }
    }

    return true;
}

static isize _parse_partitions(disk_dev* dev) {
    // Optical media is special in that it _generally_ doesn't support
    // partitioning so we treat the entire disk as a single partition
    bool is_optical = dev->type == DISK_OPTICAL;

    // Virtual drives / ram disks aren't partitioned
    bool is_virtual = dev->type == DISK_VIRTUAL;

    if (is_virtual || is_optical) {
        _create_partition(dev, -1, dev->sector_count, 0);
        return 1;
    }

    // TODO: gpt
    _parse_mbr(dev);

    return dev->partitions->size;
}

static void _mount(file_system_instance* instance) {
    disk_partition* part = instance->partition;
    file_system_interface* fs = instance->fs->fs_interface;

    vfs_node* mnt = vfs_lookup("/mnt");

    vfs_node* mount = vfs_create_node(part->name, VFS_DIR);

    fs->mount(instance, mount);

    vfs_insert_child(mnt, mount);
}

static isize _probe_partitions(disk_dev* dev) {
    for (usize i = 0; i < dev->partitions->size; i++) {
        disk_partition* part = vec_at(dev->partitions, i);

        for (usize f = 0; f < file_systems->size; f++) {
            file_system* fs = vec_at(file_systems, f);

            file_system_instance* instance = fs->fs_interface->probe(part);

            // Automount to /mnt/xxx
            if (instance) {
                instance->partition = part;
                _mount(instance);

                break;
            }
        }
    }

    return 0;
}


bool disk_register(disk_dev* dev) {
    assert(file_systems);

    dev->id = _get_disk_id();
    dev->partitions = vec_create(sizeof(disk_partition));

    log_debug("Registered new disk: %s (%zu)", dev->name, dev->id);

    usize part_count = _parse_partitions(dev);

    dump_partitions(dev);

    // this disk is a dud, there is nothibg to mount
    if (!part_count)
        return false;

    _probe_partitions(dev);

    return true;
}


// NOTE: file systems have to be Registered _before_ the disks are probed
bool file_system_register(file_system* fs) {
    if (!file_systems)
        file_systems = vec_create(sizeof(file_system));

    fs->id = _get_fs_id();

    vec_push(file_systems, fs);

    log_debug("Registered new file system: %s (%zu)", fs->name, fs->id);

    return true;
}


void dump_partitions(disk_dev* dev) {
    log_debug("Partitions found on /dev/%s:", dev->name);

    if (!dev->partitions->size)
        log_debug("[ empty table ]");

    for (usize i = 0; i < dev->partitions->size; i++) {
        disk_partition* part = vec_at(dev->partitions, i);

        log_debug("[ %s | %zu - %zu]", part->name, part->offset, part->size + part->offset);
    }
}
