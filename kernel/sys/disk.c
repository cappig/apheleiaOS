#include "disk.h"

#include <log/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbr.h"
#include "vfs.h"

static vector_t* disks = NULL;
static vector_t* file_systems = NULL;

static size_t next_disk_id = 1;
static size_t next_fs_id = 1;

static bool _vec_push_ptr(vector_t* vec, void* ptr) {
    return vec_push(vec, &ptr);
}

static void* _vec_get_ptr(vector_t* vec, size_t index) {
    void** slot = vec_at(vec, index);
    if (!slot)
        return NULL;
    return *slot;
}

static char* _strdup(const char* src) {
    if (!src)
        return NULL;

    size_t len = strlen(src);
    char* out = malloc(len + 1);

    if (!out)
        return NULL;

    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

static char* _partition_name(const char* disk, ssize_t number) {
    if (!disk)
        return NULL;

    if (number < 0)
        return _strdup(disk);

    size_t len = strlen(disk) + 4;
    char* name = calloc(len + 1, 1);

    if (!name)
        return NULL;

    snprintf(name, len + 1, "%sp%zd", disk, number);
    return name;
}

static bool _create_partition(
    disk_dev_t* dev,
    ssize_t number,
    u8 status,
    u8 type,
    size_t lba_first,
    size_t sectors
) {
    if (!dev)
        return false;

    disk_partition_t* new_part = calloc(1, sizeof(disk_partition_t));

    if (!new_part)
        return false;

    new_part->name = _partition_name(dev->name, number);
    new_part->type = type;
    new_part->status = status;
    new_part->size = sectors * dev->sector_size;
    new_part->offset = lba_first * dev->sector_size;
    new_part->disk = dev;

    if (!dev->partitions)
        dev->partitions = vec_create(sizeof(disk_partition_t*));

    if (!dev->partitions || !new_part->name || !_vec_push_ptr(dev->partitions, new_part)) {
        free(new_part->name);
        free(new_part);
        return false;
    }

    return true;
}

static bool _parse_mbr(disk_dev_t* dev) {
    if (!dev || !dev->interface || !dev->interface->read)
        return false;

    u8 mbr[512] = {0};
    ssize_t read = dev->interface->read(dev, mbr, 0, sizeof(mbr));

    if (read < (ssize_t)sizeof(mbr)) {
        log_warn("disk: failed to read MBR");
        return false;
    }

    u16 signature = (u16)mbr[510] | ((u16)mbr[511] << 8);
    if (signature != MBR_SIGNATURE) {
        log_warn("disk: invalid MBR signature");
        return false;
    }

    for (size_t i = 0; i < 4; i++) {
        size_t entry = 446 + i * 16;
        u8 status = mbr[entry + 0];
        u8 type = mbr[entry + 4];

        u32 lba_first = (u32)mbr[entry + 8] | ((u32)mbr[entry + 9] << 8) |
                        ((u32)mbr[entry + 10] << 16) | ((u32)mbr[entry + 11] << 24);
        u32 sector_count = (u32)mbr[entry + 12] | ((u32)mbr[entry + 13] << 8) |
                           ((u32)mbr[entry + 14] << 16) | ((u32)mbr[entry + 15] << 24);

        if (!type || !sector_count)
            continue;

        _create_partition(dev, (ssize_t)i, status, type, lba_first, sector_count);
    }

    return true;
}

#define GPT_SIGNATURE 0x5452415020494645ULL /* "EFI PART" */

typedef struct PACKED {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 header_crc32;
    u32 _reserved;
    u64 my_lba;
    u64 alternate_lba;
    u64 first_usable_lba;
    u64 last_usable_lba;
    u8 disk_guid[16];
    u64 partition_entry_lba;
    u32 num_partition_entries;
    u32 partition_entry_size;
    u32 partition_entry_crc32;
} gpt_header_t;

typedef struct PACKED {
    u8 type_guid[16];
    u8 unique_guid[16];
    u64 first_lba;
    u64 last_lba;
    u64 attributes;
    u16 name[36];
} gpt_entry_t;

/* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 (mixed-endian) */
static const u8 _gpt_linux_fs_guid[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4,
};

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const u8 _gpt_efi_system_guid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
};

static bool _guid_is_zero(const u8 guid[16]) {
    for (size_t i = 0; i < 16; i++) {
        if (guid[i])
            return false;
    }

    return true;
}

static u8 _gpt_type_to_mbr(const u8 guid[16]) {
    if (!memcmp(guid, _gpt_linux_fs_guid, 16))
        return MBR_LINUX;

    if (!memcmp(guid, _gpt_efi_system_guid, 16))
        return MBR_FAT32_LBA;

    return MBR_UNKNOWN;
}

static bool _parse_gpt(disk_dev_t* dev) {
    if (!dev || !dev->interface || !dev->interface->read)
        return false;

    u8 header_buf[512] = {0};
    ssize_t read = dev->interface->read(dev, header_buf, dev->sector_size, sizeof(header_buf));

    if (read < (ssize_t)sizeof(header_buf))
        return false;

    gpt_header_t* header = (gpt_header_t*)header_buf;

    if (header->signature != GPT_SIGNATURE)
        return false;

    if (!header->num_partition_entries || !header->partition_entry_size)
        return false;

    if (header->partition_entry_size < sizeof(gpt_entry_t))
        return false;

    u64 entries_lba = header->partition_entry_lba;
    u32 count = header->num_partition_entries;
    u32 entry_size = header->partition_entry_size;

    size_t entries_bytes = (size_t)count * entry_size;
    size_t entries_offset = (size_t)entries_lba * dev->sector_size;

    u8* entries = calloc(1, entries_bytes);
    if (!entries)
        return false;

    read = dev->interface->read(dev, entries, entries_offset, entries_bytes);
    if (read < (ssize_t)entries_bytes) {
        free(entries);
        return false;
    }

    ssize_t part_num = 0;

    for (u32 i = 0; i < count; i++) {
        gpt_entry_t* entry = (gpt_entry_t*)(entries + (size_t)i * entry_size);

        if (_guid_is_zero(entry->type_guid))
            continue;

        u8 mbr_type = _gpt_type_to_mbr(entry->type_guid);
        size_t first_lba = (size_t)entry->first_lba;
        size_t last_lba = (size_t)entry->last_lba;
        size_t sectors = last_lba - first_lba + 1;

        _create_partition(dev, part_num++, MBR_INACTIVE, mbr_type, first_lba, sectors);
    }

    free(entries);

    if (!part_num)
        return false;

    log_info("disk: parsed GPT with %zd partition(s)", part_num);
    return true;
}

static size_t _parse_partitions(disk_dev_t* dev) {
    if (!dev)
        return 0;

    bool is_optical = dev->type == DISK_OPTICAL;
    bool is_virtual = dev->type == DISK_VIRTUAL;

    if (is_virtual || is_optical) {
        _create_partition(dev, -1, 0, 0, 0, dev->sector_count);
        return dev->partitions ? dev->partitions->size : 0;
    }

    bool has_gpt = _parse_gpt(dev);

    if (!has_gpt)
        _parse_mbr(dev);

    if (dev->partitions && !dev->partitions->size)
        _create_partition(dev, -1, 0, 0, 0, dev->sector_count);

    return dev->partitions ? dev->partitions->size : 0;
}

static fs_instance_t* _probe_partition(disk_partition_t* part) {
    if (!part || part->fs_instance)
        return part ? part->fs_instance : NULL;

    if (!file_systems)
        return NULL;

    for (size_t i = 0; i < file_systems->size; i++) {
        fs_t* fs = _vec_get_ptr(file_systems, i);

        if (!fs || !fs->fs_interface || !fs->fs_interface->probe)
            continue;

        fs_instance_t* instance = fs->fs_interface->probe(part);

        if (instance) {
            part->fs_instance = instance;
            instance->partition = part;
            instance->filesystem = fs;
            instance->refcount = 0;
            return instance;
        }
    }

    return NULL;
}

static bool _probe_disk(disk_dev_t* dev) {
    if (!disks)
        disks = vec_create(sizeof(disk_dev_t*));

    if (!disks)
        return false;

    dev->id = next_disk_id++;

    if (!dev->partitions)
        dev->partitions = vec_create(sizeof(disk_partition_t*));

    if (!dev->partitions)
        return false;

    if (!_parse_partitions(dev))
        log_warn("disk: no partitions found on %s", dev->name ? dev->name : "disk");

    vec_insert(disks, dev->id - 1, &dev);
    return true;
}

static ssize_t _vfs_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!node || !node->private)
        return -1;

    disk_partition_t* part = node->private;

    if (!part->disk || !part->disk->interface || !part->disk->interface->read)
        return -1;

    if (part->size) {
        if (offset >= part->size)
            return 0;

        if (offset + len > part->size)
            len = part->size - offset;
    }

    return part->disk->interface->read(part->disk, buf, part->offset + offset, len);
}

static ssize_t _vfs_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!node || !node->private)
        return -1;

    disk_partition_t* part = node->private;

    if (!part->disk || !part->disk->interface || !part->disk->interface->write)
        return -1;

    if (part->size) {
        if (offset >= part->size)
            return 0;

        if (offset + len > part->size)
            len = part->size - offset;
    }

    return part->disk->interface->write(part->disk, buf, part->offset + offset, len);
}

static void _publish_partition_nodes(disk_dev_t* dev) {
    if (!dev || !dev->partitions)
        return;

    vfs_node_t* dev_dir = vfs_open("/dev", VFS_DIR, true, KDIR_MODE);

    if (!dev_dir) {
        log_warn("disk: failed to create /dev directory");
        return;
    }

    for (size_t i = 0; i < dev->partitions->size; i++) {
        disk_partition_t* part = _vec_get_ptr(dev->partitions, i);

        if (!part || !part->name)
            continue;

        char path[64] = {0};
        snprintf(path, sizeof(path), "/dev/%s", part->name);

        if (vfs_lookup(path))
            continue;

        vfs_node_t* node = vfs_create(dev_dir, part->name, VFS_BLOCKDEV, KFILE_MODE);

        if (!node) {
            log_warn("disk: failed to create /dev/%s", part->name);
            continue;
        }

        node->fs = part->fs_instance ? part->fs_instance : _probe_partition(part);
        node->private = part;
        node->interface = vfs_create_interface(_vfs_read, _vfs_write, NULL);
    }
}

static disk_partition_t* _pick_rootfs_partition(disk_dev_t* dev) {
    if (!dev || !dev->partitions)
        return NULL;

    disk_partition_t* fallback = NULL;
    disk_partition_t* linux_part = NULL;
    disk_partition_t* linux_nonboot = NULL;
    disk_partition_t* nonboot = NULL;
    size_t nonboot_offset = 0;

    for (size_t i = 0; i < dev->partitions->size; i++) {
        disk_partition_t* part = _vec_get_ptr(dev->partitions, i);

        if (!part)
            continue;

        if (!fallback)
            fallback = part;

        if (part->status != MBR_BOOTABLE) {
            if (!nonboot || part->offset > nonboot_offset) {
                nonboot = part;
                nonboot_offset = part->offset;
            }
        }

        if (part->type == MBR_LINUX) {
            if (!linux_part)
                linux_part = part;

            if (part->status != MBR_BOOTABLE && !linux_nonboot)
                linux_nonboot = part;
        }
    }

    if (linux_nonboot)
        return linux_nonboot;

    if (linux_part)
        return linux_part;

    if (nonboot)
        return nonboot;

    return fallback;
}

bool disk_register(disk_dev_t* dev) {
    if (!dev)
        return false;

    if (!_probe_disk(dev))
        return false;

    log_debug("disk: registered %s (%zu)", dev->name ? dev->name : "disk", dev->id);
    dump_partitions(dev);
    return true;
}

disk_dev_t* disk_lookup(size_t dev_id) {
    if (!disks || !dev_id)
        return NULL;

    return _vec_get_ptr(disks, dev_id - 1);
}

bool file_system_register(fs_t* fs) {
    if (!fs)
        return false;

    if (!file_systems)
        file_systems = vec_create(sizeof(fs_t*));

    if (!file_systems)
        return false;

    fs->id = next_fs_id++;

    _vec_push_ptr(file_systems, fs);
    log_debug("disk: registered file system %s (%zu)", fs->name ? fs->name : "fs", fs->id);
    return true;
}

fs_t* file_system_lookup(const char* name) {
    if (!file_systems || !name)
        return NULL;

    for (size_t i = 0; i < file_systems->size; i++) {
        fs_t* fs = _vec_get_ptr(file_systems, i);

        if (fs && fs->name && !strcmp(name, fs->name))
            return fs;
    }

    return NULL;
}

bool mount_rootfs(disk_dev_t* dev) {
    if (!dev)
        return false;

    if (!dev->partitions || !dev->partitions->size)
        return false;

    disk_partition_t* part = _pick_rootfs_partition(dev);

    if (!part) {
        log_warn("disk: no rootfs partition found");
        return false;
    }

    fs_instance_t* instance = _probe_partition(part);

    if (!instance) {
        log_warn("disk: no filesystem for %s", part->name ? part->name : "partition");
        return false;
    }

    vfs_node_t* root = vfs_lookup("/");

    if (!root) {
        log_warn("disk: missing vfs root");
        return false;
    }

    if (!vfs_mount(instance, root)) {
        log_warn("disk: failed to mount rootfs");
        return false;
    }

    log_info("disk: mounted %s at /", part->name ? part->name : "rootfs");
    return true;
}

bool disk_publish_devices(void) {
    if (!disks)
        return false;

    for (size_t i = 0; i < disks->size; i++) {
        disk_dev_t* dev = _vec_get_ptr(disks, i);
        _publish_partition_nodes(dev);
    }

    return true;
}

void dump_partitions(disk_dev_t* dev) {
    if (!dev || !dev->partitions)
        return;

    log_debug("disk: partitions on /dev/%s", dev->name ? dev->name : "disk");

    if (!dev->partitions->size) {
        log_debug("[ empty table ]");
        return;
    }

    for (size_t i = 0; i < dev->partitions->size; i++) {
        disk_partition_t* part = _vec_get_ptr(dev->partitions, i);

        if (!part)
            continue;

        unsigned long long start = (unsigned long long)part->offset;
        unsigned long long end = start + (unsigned long long)part->size;
        unsigned int type = (unsigned int)part->type;
        log_debug(
            "[ %s | %llu - %llu | type=0x%02x ]", part->name ? part->name : "-", start, end, type
        );
    }
}
