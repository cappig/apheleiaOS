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
static vector* disks = NULL;


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

    assert(dev->partitions);

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

static bool _parse_gpt(disk_dev* dev) {
    // TODO: implement this
    return false;
}

static isize _parse_partitions(disk_dev* dev) {
    // Optical media is special in that it _generally_ doesn't support
    // partitioning so we treat the entire disk as a single partition
    bool is_optical = dev->type == DISK_OPTICAL;

    // Virtual drives / ramdisks also aren't partitioned
    bool is_virtual = dev->type == DISK_VIRTUAL;

    if (is_virtual || is_optical) {
        _create_partition(dev, -1, dev->sector_count, 0);
        return 1;
    }

    // GPT takes precedence if present
    bool has_gpt = _parse_gpt(dev);

    if (!has_gpt)
        _parse_mbr(dev);

    return dev->partitions->size;
}

static file_system_instance* _probe_partition(disk_partition* part) {
    if (part->fs_instance)
        return part->fs_instance;

    // FIXME: this seems retarded, we should pass in the expected fs
    for (usize f = 0; f < file_systems->size; f++) {
        file_system* fs = vec_at(file_systems, f);

        if (!fs || !fs->fs_interface || !fs->fs_interface->probe)
            continue;

        file_system_instance* instance = fs->fs_interface->probe(part);

        if (instance) {
            part->fs_instance = instance;

            instance->partition = part;
            instance->fs = fs;
            instance->refcount = 0;

            return instance;
        }
    }

    return NULL;
}

static isize _mount_partitions(disk_dev* dev) {
    vfs_node* dev_dir = vfs_open("/dev", VFS_DIR, true, KDIR_MODE);

    isize num = 0;
    for (usize i = 0; i < dev->partitions->size; i++) {
        disk_partition* part = vec_at(dev->partitions, i);

        file_system_instance* instance = _probe_partition(part);

        if (!instance)
            continue;

        vfs_node* node = vfs_create(dev_dir, part->name, VFS_BLOCKDEV, KFILE_MODE);

        if (!node) {
            log_warn("Failed to create disk device: /dev/%s", part->name);
            continue;
        }

        node->fs = instance;

        num++;
    }

    return num;
}

static bool _probe_disk(disk_dev* dev) {
    if (!disks)
        disks = vec_create(sizeof(disk_dev));

    dev->id = _get_disk_id();

    if (!dev->partitions)
        dev->partitions = vec_create(sizeof(disk_partition));

    if (!_parse_partitions(dev))
        return false;

    vec_insert(disks, dev->id - 1, dev);

    return true;
}

bool disk_register(disk_dev* dev) {
    if (!_probe_disk(dev))
        return false;

    _mount_partitions(dev);

    log_debug("Registered new disk: %s (%zu)", dev->name, dev->id);

    dump_partitions(dev);

    return true;
}

disk_dev* disk_lookup(usize dev_id) {
    if (!disks || !dev_id)
        return NULL;

    return vec_at(disks, dev_id - 1);
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

file_system* file_system_lookup(const char* name) {
    assert(file_systems);

    for (usize i = 0; i < file_systems->size; i++) {
        file_system* fs = vec_at(file_systems, i);

        if (!strcmp(name, fs->name))
            return fs;
    }

    return NULL;
}


void mount_rootfs(disk_dev* dev) {
    _probe_disk(dev);

    assert(dev->partitions);

    disk_partition* part = vec_at(dev->partitions, 0);

    file_system_instance* instance = _probe_partition(part);
    assert(instance);

    vfs_node* root = vfs_lookup("/");
    vfs_mount(instance, root);

    log_info("Mounted %s at /", dev->name);
}


void dump_partitions(disk_dev* dev) {
    log_debug("Partitions found on /dev/%s:", dev->name);

    if (!dev->partitions->size)
        log_debug("[ empty table ]");

    for (usize i = 0; i < dev->partitions->size; i++) {
        disk_partition* part = vec_at(dev->partitions, i);

        file_system_instance* instance = part->fs_instance;
        char* fs_name = instance ? instance->fs->name : "-";

        usize end = part->size + part->offset;
        log_debug("[ %s | %zu - %zu | %s ]", part->name, part->offset, end, fs_name);
    }
}
