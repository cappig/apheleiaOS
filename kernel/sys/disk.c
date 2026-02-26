#include "disk.h"

#include <fs/ext2.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devfs.h"
#include "mbr.h"
#include "vfs.h"

static vector_t *disks = NULL;
static vector_t *file_systems = NULL;

static size_t next_disk_id = 1;
static size_t next_fs_id = 1;
static bool preferred_rootfs_uuid_set = false;
static u8 preferred_rootfs_uuid[16] = {0};
static bool boot_hint_set = false;
static disk_boot_hint_t boot_hint = {0};

static bool _disk_name_exists(const char *name) {
    if (!name || !name[0] || !disks) {
        return false;
    }

    for (size_t i = 0; i < disks->size; i++) {
        disk_dev_t *existing = vec_at_ptr(disks, i);

        if (!existing || !existing->name) {
            continue;
        }

        if (!strcmp(existing->name, name)) {
            return true;
        }
    }

    return false;
}

static void _destroy_fs_instance(fs_instance_t *instance) {
    if (!instance) {
        return;
    }

    if (
        instance->has_tree &&
        instance->filesystem &&
        instance->filesystem->fs_interface &&
        instance->filesystem->fs_interface->destroy_tree
    ) {
        (void)instance->filesystem->fs_interface->destroy_tree(instance);
    }

    if (instance->private) {
        free(instance->private);
    }

    free(instance);
}

static void _destroy_partitions(disk_dev_t *dev) {
    if (!dev || !dev->partitions) {
        return;
    }

    for (size_t i = 0; i < dev->partitions->size; i++) {
        disk_partition_t *part = vec_at_ptr(dev->partitions, i);
        if (!part) {
            continue;
        }

        _destroy_fs_instance(part->fs_instance);
        part->fs_instance = NULL;

        free(part->name);
        free(part);
    }

    vec_destroy(dev->partitions);
    dev->partitions = NULL;
}

static const char *_disk_name_prefix(size_t type) {
    switch (type) {
    case DISK_VIRTUAL:
        return "ram";
    case DISK_HARD:
        return "sd";
    case DISK_FLOPPY:
        return "fd";
    case DISK_OPTICAL:
        return "cd";
    case DISK_USB:
        return "usb";
    default:
        return "disk";
    }
}

static char *_alloc_unique_disk_name(size_t type) {
    const char *prefix = _disk_name_prefix(type);

    for (size_t index = 0; index < 1024; index++) {
        char candidate[32] = {0};
        int wrote = snprintf(
            candidate,
            sizeof(candidate),
            "%s%zu",
            prefix,
            index
        );

        if (wrote <= 0 || (size_t)wrote >= sizeof(candidate)) {
            continue;
        }

        if (_disk_name_exists(candidate)) {
            continue;
        }

        return strdup(candidate);
    }

    return NULL;
}

static bool _ensure_disk_name(disk_dev_t *dev) {
    if (!dev) {
        return false;
    }

    bool has_name = dev->name && dev->name[0];
    if (has_name && !_disk_name_exists(dev->name)) {
        return true;
    }

    char *old = dev->name;
    char *name = _alloc_unique_disk_name(dev->type);
    if (!name) {
        return false;
    }

    dev->name = name;

    if (has_name) {
        log_warn("disk name '%s' already exists, renamed to '%s'", old, name);
    }

    return true;
}

static char *_partition_name(const char *disk, ssize_t number) {
    if (!disk) {
        return NULL;
    }

    if (number < 0) {
        return strdup(disk);
    }

    size_t len = strlen(disk) + 4;
    char *name = calloc(len + 1, 1);

    if (!name) {
        return NULL;
    }

    snprintf(name, len + 1, "%sp%zd", disk, number);
    return name;
}

static bool _partition_is_whole_disk_alias(const disk_partition_t *part) {
    if (!part || !part->disk || !part->name || !part->disk->name) {
        return false;
    }

    return !strcmp(part->name, part->disk->name);
}

static bool _disk_has_real_partitions(const disk_dev_t *dev) {
    if (!dev || !dev->partitions) {
        return false;
    }

    for (size_t i = 0; i < dev->partitions->size; i++) {
        disk_partition_t *part = vec_at_ptr(dev->partitions, i);

        if (!part || _partition_is_whole_disk_alias(part)) {
            continue;
        }

        return true;
    }

    return false;
}

static bool _create_partition(
    disk_dev_t *dev,
    ssize_t number,
    u8 status,
    u8 type,
    size_t lba_first,
    size_t sectors
) {
    if (!dev) {
        return false;
    }

    disk_partition_t *new_part = calloc(1, sizeof(disk_partition_t));

    if (!new_part) {
        return false;
    }

    new_part->name = _partition_name(dev->name, number);
    new_part->type = type;
    new_part->status = status;
    new_part->size = sectors * dev->sector_size;
    new_part->offset = lba_first * dev->sector_size;
    new_part->disk = dev;

    if (!dev->partitions) {
        dev->partitions = vec_create(sizeof(disk_partition_t *));
    }

    if (!dev->partitions || !new_part->name || !vec_push(dev->partitions, &new_part)) {
        free(new_part->name);
        free(new_part);
        return false;
    }

    return true;
}

static bool _partition_ext2_uuid(disk_partition_t *part, u8 out_uuid[16]) {
    if (
        !part ||
        !out_uuid ||
        !part->disk ||
        !part->disk->interface ||
        !part->disk->interface->read
    ) {
        return false;
    }

    if (part->size < sizeof(ext2_superblock_t) + 1024) {
        return false;
    }

    if (part->offset > (size_t)-1 - 1024) {
        return false;
    }

    ext2_superblock_t sb = {0};
    ssize_t read = part->disk->interface->read(
        part->disk,
        &sb,
        part->offset + 1024,
        sizeof(sb)
    );

    if (read < (ssize_t)sizeof(sb)) {
        return false;
    }

    if (sb.signature != EXT2_SIGNATURE) {
        return false;
    }

    memcpy(out_uuid, sb.fs_id, 16);
    return true;
}

static bool _partition_matches_preferred_rootfs(disk_partition_t *part) {
    if (!preferred_rootfs_uuid_set) {
        return false;
    }

    u8 uuid[16] = {0};
    if (!_partition_ext2_uuid(part, uuid)) {
        return false;
    }

    return !memcmp(uuid, preferred_rootfs_uuid, sizeof(uuid));
}

static bool _disk_matches_boot_hint_transport(const disk_dev_t *dev) {
    if (!dev || !boot_hint_set || !boot_hint.valid) {
        return false;
    }

    switch (boot_hint.transport) {
    case DISK_BOOT_TRANSPORT_USB:
        return dev->type == DISK_USB;
    case DISK_BOOT_TRANSPORT_ATAPI:
        return dev->type == DISK_OPTICAL;
    case DISK_BOOT_TRANSPORT_ATA:
    case DISK_BOOT_TRANSPORT_AHCI:
    case DISK_BOOT_TRANSPORT_NVME:
        return dev->type == DISK_HARD;
    default:
        break;
    }

    switch (boot_hint.media) {
    case DISK_BOOT_MEDIA_USB:
        return dev->type == DISK_USB;
    case DISK_BOOT_MEDIA_OPTICAL:
        return dev->type == DISK_OPTICAL;
    case DISK_BOOT_MEDIA_DISK:
        return dev->type == DISK_HARD;
    default:
        return false;
    }
}

static bool _has_transport_hint(void) {
    if (!boot_hint_set || !boot_hint.valid) {
        return false;
    }

    if (boot_hint.transport != DISK_BOOT_TRANSPORT_UNKNOWN) {
        return true;
    }

    return boot_hint.media != DISK_BOOT_MEDIA_UNKNOWN;
}

static bool _parse_mbr(disk_dev_t *dev) {
    if (!dev || !dev->interface || !dev->interface->read) {
        return false;
    }

    u8 mbr[512] = {0};
    ssize_t read = dev->interface->read(dev, mbr, 0, sizeof(mbr));

    if (read < (ssize_t)sizeof(mbr)) {
        log_warn("failed to read MBR");
        return false;
    }

    u16 signature = (u16)mbr[510] | ((u16)mbr[511] << 8);
    if (signature != MBR_SIGNATURE) {
        log_warn("invalid MBR signature");
        return false;
    }

    for (size_t i = 0; i < 4; i++) {
        size_t entry = 446 + i * 16;
        u8 status = mbr[entry + 0];
        u8 type = mbr[entry + 4];

        u32 lba_first =
            (u32)mbr[entry + 8] | ((u32)mbr[entry + 9] << 8) |
            ((u32)mbr[entry + 10] << 16) | ((u32)mbr[entry + 11] << 24);

        u32 sector_count =
            (u32)mbr[entry + 12] | ((u32)mbr[entry + 13] << 8) |
            ((u32)mbr[entry + 14] << 16) | ((u32)mbr[entry + 15] << 24);

        if (!type || !sector_count) {
            continue;
        }

        _create_partition(
            dev, (ssize_t)i, status, type, lba_first, sector_count
        );
    }

    return true;
}

// "EFI PART"
#define GPT_SIGNATURE 0x5452415020494645ULL

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

// 0FC63DAF-8483-4772-8E79-3D69D8477DE4 (mixed-endian)
static const u8 _gpt_linux_fs_guid[16] = {
    0xAF,
    0x3D,
    0xC6,
    0x0F,
    0x83,
    0x84,
    0x72,
    0x47,
    0x8E,
    0x79,
    0x3D,
    0x69,
    0xD8,
    0x47,
    0x7D,
    0xE4,
};

// C12A7328-F81F-11D2-BA4B-00A0C93EC93B
static const u8 _gpt_efi_system_guid[16] = {
    0x28,
    0x73,
    0x2A,
    0xC1,
    0x1F,
    0xF8,
    0xD2,
    0x11,
    0xBA,
    0x4B,
    0x00,
    0xA0,
    0xC9,
    0x3E,
    0xC9,
    0x3B,
};

// 21686148-6449-6E6F-744E-656564454649
static const u8 _gpt_bios_boot_guid[16] = {
    0x48,
    0x61,
    0x68,
    0x21,
    0x49,
    0x64,
    0x6F,
    0x6E,
    0x74,
    0x4E,
    0x65,
    0x65,
    0x64,
    0x45,
    0x46,
    0x49,
};

static bool _guid_is_zero(const u8 guid[16]) {
    for (size_t i = 0; i < 16; i++) {
        if (guid[i]) {
            return false;
        }
    }

    return true;
}

static u8 _gpt_type_to_mbr(const u8 guid[16]) {
    if (!memcmp(guid, _gpt_linux_fs_guid, 16)) {
        return MBR_LINUX;
    }

    if (!memcmp(guid, _gpt_efi_system_guid, 16)) {
        return MBR_FAT32_LBA;
    }

    return MBR_UNKNOWN;
}

static bool _parse_gpt(disk_dev_t *dev) {
    if (!dev || !dev->interface || !dev->interface->read) {
        return false;
    }

    u8 header_buf[512] = {0};
    ssize_t read = dev->interface->read(
        dev, header_buf, dev->sector_size, sizeof(header_buf)
    );

    if (read < (ssize_t)sizeof(header_buf)) {
        return false;
    }

    gpt_header_t *header = (gpt_header_t *)header_buf;

    if (header->signature != GPT_SIGNATURE) {
        return false;
    }

    if (!header->num_partition_entries || !header->partition_entry_size) {
        return false;
    }

    if (header->partition_entry_size < sizeof(gpt_entry_t)) {
        return false;
    }

    u64 entries_lba = header->partition_entry_lba;
    u32 count = header->num_partition_entries;
    u32 entry_size = header->partition_entry_size;

    size_t entries_bytes = (size_t)count * entry_size;
    size_t entries_offset = (size_t)entries_lba * dev->sector_size;

    u8 *entries = calloc(1, entries_bytes);
    if (!entries) {
        return false;
    }

    read = dev->interface->read(dev, entries, entries_offset, entries_bytes);
    if (read < (ssize_t)entries_bytes) {
        free(entries);
        return false;
    }

    ssize_t part_num = 0;

    for (u32 i = 0; i < count; i++) {
        gpt_entry_t *entry = (gpt_entry_t *)(entries + (size_t)i * entry_size);

        if (_guid_is_zero(entry->type_guid)) {
            continue;
        }

        if (!memcmp(entry->type_guid, _gpt_bios_boot_guid, 16)) {
            continue;
        }

        u8 mbr_type = _gpt_type_to_mbr(entry->type_guid);
        size_t first_lba = (size_t)entry->first_lba;
        size_t last_lba = (size_t)entry->last_lba;
        size_t sectors = last_lba - first_lba + 1;

        _create_partition(
            dev, part_num++, MBR_INACTIVE, mbr_type, first_lba, sectors
        );
    }

    free(entries);

    if (!part_num) {
        return false;
    }

    log_debug("parsed GPT with %zd partition(s)", part_num);
    return true;
}

static size_t _parse_partitions(disk_dev_t *dev) {
    if (!dev) {
        return 0;
    }

    if (dev->type == DISK_VIRTUAL) {
        _create_partition(dev, -1, 0, 0, 0, dev->sector_count);
        return dev->partitions ? dev->partitions->size : 0;
    }

    // Always expose /dev/<disk> in addition to partition nodes
    (void)_create_partition(dev, -1, 0, 0, 0, dev->sector_count);

    if (!_parse_gpt(dev)) {
        _parse_mbr(dev);
    }

    if (!dev->partitions || !dev->partitions->size) {
        _create_partition(dev, -1, 0, 0, 0, dev->sector_count);
    }

    return dev->partitions ? dev->partitions->size : 0;
}

static fs_instance_t *_probe_partition(disk_partition_t *part) {
    if (!part || part->fs_instance) {
        return part ? part->fs_instance : NULL;
    }

    if (!file_systems) {
        return NULL;
    }

    for (size_t i = 0; i < file_systems->size; i++) {
        fs_t *fs = vec_at_ptr(file_systems, i);

        if (!fs || !fs->fs_interface || !fs->fs_interface->probe) {
            continue;
        }

        fs_instance_t *instance = fs->fs_interface->probe(part);

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

static bool _probe_disk(disk_dev_t *dev) {
    if (!disks) {
        disks = vec_create(sizeof(disk_dev_t *));
    }

    if (!disks) {
        return false;
    }

    dev->id = next_disk_id++;

    if (!dev->partitions) {
        dev->partitions = vec_create(sizeof(disk_partition_t *));
    }

    if (!dev->partitions) {
        return false;
    }

    if (!_parse_partitions(dev)) {
        log_warn("no partitions found on %s", dev->name ? dev->name : "disk");
    }

    vec_insert(disks, dev->id - 1, &dev);
    return true;
}

static ssize_t
_vfs_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!node || !node->private) {
        return -1;
    }

    disk_partition_t *part = node->private;

    if (!part->disk || !part->disk->interface || !part->disk->interface->read) {
        return -1;
    }

    if (part->size) {
        if (offset >= part->size) {
            return 0;
        }

        if (offset + len > part->size) {
            len = part->size - offset;
        }
    }

    return part->disk->interface->read(
        part->disk, buf, part->offset + offset, len
    );
}

static ssize_t
_vfs_write(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!node || !node->private) {
        return -1;
    }

    disk_partition_t *part = node->private;

    if (!part->disk || !part->disk->interface || !part->disk->interface->write) {
        return -1;
    }

    if (part->size) {
        if (offset >= part->size) {
            return 0;
        }

        if (offset + len > part->size) {
            len = part->size - offset;
        }
    }

    return part->disk->interface->write(
        part->disk, buf, part->offset + offset, len
    );
}

static void _publish_partition_nodes(disk_dev_t *dev) {
    if (!dev || !dev->partitions) {
        return;
    }

    vfs_node_t *dev_dir = vfs_lookup("/dev");
    if (!dev_dir) {
        vfs_node_t *root = vfs_lookup("/");
        if (root && VFS_IS_LINK(root->type) && root->link) {
            root = root->link;
        }

        if (root) {
            dev_dir = vfs_create_virtual(root, "dev", VFS_DIR, KDIR_MODE);
        }
    }

    if (!dev_dir) {
        log_warn("failed to create /dev directory");
        return;
    }

    for (size_t i = 0; i < dev->partitions->size; i++) {
        disk_partition_t *part = vec_at_ptr(dev->partitions, i);

        if (!part || !part->name) {
            continue;
        }

        char path[64] = {0};
        snprintf(path, sizeof(path), "/dev/%s", part->name);

        if (vfs_lookup(path)) {
            continue;
        }

        vfs_node_t *node =
            vfs_create_virtual(dev_dir, part->name, VFS_BLOCKDEV, KFILE_MODE);

        if (!node) {
            log_warn("failed to create /dev/%s", part->name);
            continue;
        }

        node->fs =
            part->fs_instance ? part->fs_instance : _probe_partition(part);

        node->private = part;
        node->interface = vfs_create_interface(_vfs_read, _vfs_write, NULL);
    }
}

static disk_partition_t *_pick_rootfs_partition(
    disk_dev_t *dev,
    bool honor_preferred_uuid
) {
    if (!dev || !dev->partitions) {
        return NULL;
    }

    bool has_real = _disk_has_real_partitions(dev);

    if (
        honor_preferred_uuid &&
        dev->type != DISK_VIRTUAL &&
        preferred_rootfs_uuid_set
    ) {
        for (size_t i = 0; i < dev->partitions->size; i++) {
            disk_partition_t *part = vec_at_ptr(dev->partitions, i);
            if (!part) {
                continue;
            }

            if (has_real && _partition_is_whole_disk_alias(part)) {
                continue;
            }

            if (_partition_matches_preferred_rootfs(part)) {
                return part;
            }
        }

        return NULL;
    }

    disk_partition_t *fallback = NULL;
    disk_partition_t *linux_part = NULL;
    disk_partition_t *linux_nonboot = NULL;
    disk_partition_t *nonboot = NULL;
    size_t nonboot_offset = 0;

    for (size_t i = 0; i < dev->partitions->size; i++) {
        disk_partition_t *part = vec_at_ptr(dev->partitions, i);

        if (!part) {
            continue;
        }

        if (has_real && _partition_is_whole_disk_alias(part)) {
            continue;
        }

        if (!fallback) {
            fallback = part;
        }

        if (part->status != MBR_BOOTABLE) {
            if (!nonboot || part->offset > nonboot_offset) {
                nonboot = part;
                nonboot_offset = part->offset;
            }
        }

        if (part->type == MBR_LINUX) {
            if (!linux_part) {
                linux_part = part;
            }

            if (part->status != MBR_BOOTABLE && !linux_nonboot) {
                linux_nonboot = part;
            }
        }
    }

    if (linux_nonboot) {
        return linux_nonboot;
    }

    if (linux_part) {
        return linux_part;
    }

    if (nonboot) {
        return nonboot;
    }

    return fallback;
}

bool disk_register(disk_dev_t *dev) {
    if (!dev) {
        return false;
    }

    if (!_ensure_disk_name(dev)) {
        return false;
    }

    if (!_probe_disk(dev)) {
        return false;
    }

    if (devfs_is_ready()) {
        _publish_partition_nodes(dev);
    }

    log_debug("registered %s (%zu)", dev->name ? dev->name : "disk", dev->id);
    dump_partitions(dev);
    return true;
}

bool disk_is_busy(const disk_dev_t *dev) {
    if (!dev || !dev->partitions) {
        return false;
    }

    for (size_t i = 0; i < dev->partitions->size; i++) {
        disk_partition_t *part = vec_at_ptr(dev->partitions, i);
        if (!part) {
            continue;
        }

        if (part->fs_instance && part->fs_instance->refcount) {
            return true;
        }

        if (!part->name) {
            continue;
        }

        char path[64] = {0};
        snprintf(path, sizeof(path), "/dev/%s", part->name);

        vfs_node_t *node = vfs_lookup(path);
        if (node && sched_fd_refs_node(node)) {
            return true;
        }
    }

    return false;
}

bool disk_unregister(disk_dev_t *dev) {
    if (!dev || !disks) {
        return false;
    }

    if (disk_is_busy(dev)) {
        return false;
    }

    if (dev->partitions) {
        for (size_t i = 0; i < dev->partitions->size; i++) {
            disk_partition_t *part = vec_at_ptr(dev->partitions, i);

            if (!part || !part->name) {
                continue;
            }

            char path[64] = {0};
            snprintf(path, sizeof(path), "/dev/%s", part->name);

            vfs_node_t *node = vfs_lookup(path);
            if (!node) {
                continue;
            }

            if (!vfs_unlink(path)) {
                return false;
            }
        }
    }

    bool removed = false;

    for (size_t i = 0; i < disks->size; i++) {
        disk_dev_t **slot = vec_at(disks, i);
        if (!slot || *slot != dev) {
            continue;
        }

        *slot = NULL;
        removed = true;
        break;
    }

    if (!removed) {
        return false;
    }

    _destroy_partitions(dev);
    dev->id = 0;

    return true;
}

disk_dev_t *disk_lookup(size_t dev_id) {
    if (!disks || !dev_id) {
        return NULL;
    }

    return vec_at_ptr(disks, dev_id - 1);
}

bool file_system_register(fs_t *fs) {
    if (!fs) {
        return false;
    }

    if (!file_systems) {
        file_systems = vec_create(sizeof(fs_t *));
    }

    if (!file_systems) {
        return false;
    }

    fs->id = next_fs_id++;

    vec_push(file_systems, &fs);
    log_debug(
        "registered file system %s (%zu)", fs->name ? fs->name : "fs", fs->id
    );
    return true;
}

fs_t *file_system_lookup(const char *name) {
    if (!file_systems || !name) {
        return NULL;
    }

    for (size_t i = 0; i < file_systems->size; i++) {
        fs_t *fs = vec_at_ptr(file_systems, i);

        if (fs && fs->name && !strcmp(name, fs->name)) {
            return fs;
        }
    }

    return NULL;
}

bool disk_mount_partition_node(
    vfs_node_t *source,
    vfs_node_t *target,
    const char *fs_name
) {
    if (!source || !target) {
        return false;
    }

    if (source->type != VFS_BLOCKDEV || !source->private) {
        return false;
    }

    if (target->type != VFS_DIR) {
        return false;
    }

    if (!fs_name || !fs_name[0]) {
        fs_name = "ext2";
    }

    disk_partition_t *part = source->private;
    fs_instance_t *instance = part->fs_instance;

    if (
        instance &&
        instance->filesystem &&
        instance->filesystem->name &&
        strcmp(instance->filesystem->name, fs_name)
    ) {
        instance = NULL;
    }

    if (!instance) {
        fs_t *fs = file_system_lookup(fs_name);
        if (!fs || !fs->fs_interface || !fs->fs_interface->probe) {
            return false;
        }

        instance = fs->fs_interface->probe(part);
        if (!instance) {
            return false;
        }

        part->fs_instance = instance;
        instance->partition = part;
        instance->filesystem = fs;
        instance->refcount = 0;
    }

    return vfs_mount(instance, target);
}

bool disk_unmount_node(vfs_node_t *target, bool destroy_tree) {
    if (!target || target->type != VFS_MOUNT) {
        return false;
    }

    return vfs_unmount(target, destroy_tree);
}

void disk_clear_preferred_rootfs_uuid(void) {
    memset(preferred_rootfs_uuid, 0, sizeof(preferred_rootfs_uuid));
    preferred_rootfs_uuid_set = false;
}

void disk_set_preferred_rootfs_uuid(const u8 uuid[16]) {
    if (!uuid) {
        disk_clear_preferred_rootfs_uuid();
        return;
    }

    memcpy(preferred_rootfs_uuid, uuid, sizeof(preferred_rootfs_uuid));
    preferred_rootfs_uuid_set = true;
}

void disk_clear_boot_hint(void) {
    boot_hint_set = false;
    memset(&boot_hint, 0, sizeof(boot_hint));
}

void disk_set_boot_hint(const disk_boot_hint_t *hint) {
    if (!hint || !hint->valid) {
        disk_clear_boot_hint();
        return;
    }

    boot_hint = *hint;
    boot_hint_set = true;
}

static bool _mount_partition_as_root(vfs_node_t *root, disk_partition_t *part) {
    if (!root || !part) {
        return false;
    }

    fs_instance_t *instance = _probe_partition(part);
    if (!instance) {
        return false;
    }

    if (!vfs_mount(instance, root)) {
        return false;
    }

    log_info("mounted %s at /", part->name ? part->name : "rootfs");
    return true;
}

static bool _mount_rootfs_on_disk(disk_dev_t *dev, bool honor_preferred_uuid) {
    if (!dev) {
        return false;
    }

    if (!dev->partitions || !dev->partitions->size) {
        return false;
    }

    disk_partition_t *preferred = _pick_rootfs_partition(dev, honor_preferred_uuid);

    if (!preferred) {
        if (
            honor_preferred_uuid &&
            preferred_rootfs_uuid_set &&
            dev->type != DISK_VIRTUAL
        ) {
            log_debug(
                "no preferred rootfs UUID match on %s",
                dev->name ? dev->name : "disk"
            );
        } else {
            log_debug(
                "no rootfs candidate on %s",
                dev->name ? dev->name : "disk"
            );
        }

        return false;
    }

    vfs_node_t *root = vfs_lookup("/");

    if (!root) {
        log_warn("missing vfs root");
        return false;
    }

    if (_mount_partition_as_root(root, preferred)) {
        return true;
    }

    if (!_probe_partition(preferred)) {
        log_warn(
            "no filesystem for %s",
            preferred->name ? preferred->name : "partition"
        );
    }

    bool has_real = _disk_has_real_partitions(dev);

    for (size_t i = 0; i < dev->partitions->size; i++) {
        disk_partition_t *part = vec_at_ptr(dev->partitions, i);

        if (!part || part == preferred) {
            continue;
        }

        if (has_real && _partition_is_whole_disk_alias(part)) {
            continue;
        }

        if (_mount_partition_as_root(root, part)) {
            return true;
        }
    }

    log_debug(
        "failed to mount rootfs from %s",
        dev->name ? dev->name : "disk"
    );
    return false;
}

bool mount_rootfs(void) {
    if (!disks || !disks->size) {
        return false;
    }

    bool has_virtual = false;

    for (size_t i = 0; i < disks->size; i++) {
        disk_dev_t *dev = vec_at_ptr(disks, i);

        if (dev && dev->type == DISK_VIRTUAL) {
            has_virtual = true;
            break;
        }
    }

    if (preferred_rootfs_uuid_set) {
        for (size_t i = 0; i < disks->size; i++) {
            disk_dev_t *dev = vec_at_ptr(disks, i);

            if (!dev || dev->type == DISK_VIRTUAL) {
                continue;
            }

            if (_mount_rootfs_on_disk(dev, true)) {
                return true;
            }
        }

        if (has_virtual) {
            for (size_t i = 0; i < disks->size; i++) {
                disk_dev_t *dev = vec_at_ptr(disks, i);
                if (!dev || dev->type != DISK_VIRTUAL) {
                    continue;
                }

                if (_mount_rootfs_on_disk(dev, true)) {
                    return true;
                }
            }

            return false;
        }

        log_warn(
            "preferred rootfs UUID set but no staged fallback disk is present; "
            "probing all physical disks"
        );
    }

    if (_has_transport_hint()) {
        for (size_t i = 0; i < disks->size; i++) {
            disk_dev_t *dev = vec_at_ptr(disks, i);

            if (!dev || dev->type == DISK_VIRTUAL) {
                continue;
            }

            if (!_disk_matches_boot_hint_transport(dev)) {
                continue;
            }

            if (_mount_rootfs_on_disk(dev, false)) {
                return true;
            }
        }

        for (size_t i = 0; i < disks->size; i++) {
            disk_dev_t *dev = vec_at_ptr(disks, i);

            if (!dev || dev->type == DISK_VIRTUAL) {
                continue;
            }

            if (_disk_matches_boot_hint_transport(dev)) {
                continue;
            }

            if (_mount_rootfs_on_disk(dev, false)) {
                return true;
            }
        }
    } else {
        for (size_t i = 0; i < disks->size; i++) {
            disk_dev_t *dev = vec_at_ptr(disks, i);

            if (!dev || dev->type == DISK_VIRTUAL) {
                continue;
            }

            if (_mount_rootfs_on_disk(dev, false)) {
                return true;
            }
        }
    }

    for (size_t i = 0; i < disks->size; i++) {
        disk_dev_t *dev = vec_at_ptr(disks, i);

        if (!dev || dev->type != DISK_VIRTUAL) {
            continue;
        }

        if (_mount_rootfs_on_disk(dev, false)) {
            return true;
        }
    }

    return false;
}

bool disk_publish_devices(void) {
    if (!disks) {
        return false;
    }

    for (size_t i = 0; i < disks->size; i++) {
        disk_dev_t *dev = vec_at_ptr(disks, i);
        _publish_partition_nodes(dev);
    }

    return true;
}

void dump_partitions(disk_dev_t *dev) {
    if (!dev || !dev->partitions) {
        return;
    }

    log_debug("partitions on /dev/%s", dev->name ? dev->name : "disk");

    if (!dev->partitions->size) {
        log_debug("[ empty table ]");
        return;
    }

    for (size_t i = 0; i < dev->partitions->size; i++) {
        disk_partition_t *part = vec_at_ptr(dev->partitions, i);

        if (!part) {
            continue;
        }

        unsigned long long start = (unsigned long long)part->offset;
        unsigned long long end = start + (unsigned long long)part->size;
        unsigned int type = (unsigned int)part->type;
        log_debug(
            "[ %s | %llu - %llu | type=0x%02x ]",
            part->name ? part->name : "-",
            start,
            end,
            type
        );
    }
}
