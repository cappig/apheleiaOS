#include "disk.h"

#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <common/ext2.h>

#include "bios.h"
#include "memory.h"
#include "tty.h"
#include "x86/boot.h"
#include "x86/mbr.h"
#include "x86/regs.h"

#define MBR_SECTOR_SIZE 512

#define MAX_BIOS_SECTORS_PER_CALL 64
#define BOUNCE_SIZE               8192

static u16 disk_code = 0;
static u16 disk_sector_size = MBR_SECTOR_SIZE;
static u16 disk_flags = 0;
static size_t rootfs_base = 0;
static size_t rootfs_size = 0;
static mbr_partition_t rootfs_partition = {0};
static bool rootfs_partition_valid = false;
static u8 rootfs_partition_index = 0;
static bool _read_rootfs_bytes(
    void *dest,
    size_t offset,
    size_t bytes,
    void *ctx
);

static boot_ext2_t rootfs = {0};
static u8 bounce[BOUNCE_SIZE] = {0};


static bool _bios_read_lba(void *dest, size_t lba, u16 sectors) {
    if (!dest || !sectors) {
        return false;
    }

    u32 real_dest = (u32)REAL_OFF(dest) | ((u32)REAL_SEG(dest) << 16);

    dap_t dap = {
        .size = sizeof(dap_t),
        .sectors = sectors,
        .destination = real_dest,
        .lba = lba,
    };

    regs32_t r = {0};
    r.ah = 0x42;
    r.dl = disk_code;
    r.esi = (uintptr_t)&dap;

    bios_call(0x13, &r, &r);
    return !(r.flags & FLAG_CF);
}

int read_disk(void *dest, size_t offset, size_t bytes) {
    size_t ss = disk_sector_size;
    size_t lba = offset / ss;
    size_t sector_off = offset % ss;

    u16 bounce_sectors = BOUNCE_SIZE / ss;
    u16 max_sectors =
        (u16)min((size_t)MAX_BIOS_SECTORS_PER_CALL, (size_t)bounce_sectors);

    uint8_t *out = dest;

    while (bytes > 0) {
        size_t bytes_window = bytes + sector_off;
        size_t sectors_window = DIV_ROUND_UP(bytes_window, ss);
        u16 sectors = (u16)min(sectors_window, (size_t)max_sectors);

        if (!_bios_read_lba(bounce, lba, sectors)) {
            panic("disk read error");
        }

        size_t available = (size_t)sectors * ss - sector_off;
        size_t to_copy = min(bytes, available);

        memcpy(out, bounce + sector_off, to_copy);

        out += to_copy;
        bytes -= to_copy;
        lba += sectors;
        sector_off = 0;
    }

    return 0;
}

static bool _find_rootfs(mbr_partition_t *out_part, u8 *part_index) {
    mbr_t mbr;

    read_disk(&mbr, 0, sizeof(mbr_t));

    if (mbr.signature != MBR_SIGNATURE) {
        panic("MBR has invalid signature");
    }

    for (size_t i = 0; i < 4; i++) {
        mbr_partition_t *partition = &mbr.table.partitions[i];

        if (partition->type != MBR_LINUX) {
            continue;
        }

        if (partition->status == MBR_BOOTABLE) {
            continue;
        }

        memcpy(out_part, partition, sizeof(mbr_partition_t));
        if (part_index) {
            *part_index = (u8)i;
        }

        return true;
    }

    return false;
}

static void _detect_sector_size(void) {
    disk_params_t params;
    memset(&params, 0, sizeof(params));
    params.size = sizeof(params);

    regs32_t r = {0};
    r.ah = 0x48;
    r.dl = disk_code;
    r.esi = (uintptr_t)&params;

    bios_call(0x13, &r, &r);

    if (!(r.flags & FLAG_CF)) {
        disk_flags = params.flags;
        if (params.bytes_per_sector >= 512) {
            disk_sector_size = params.bytes_per_sector;
        }
    }
}

void disk_init(u16 disk) {
    disk_code = disk;
    _detect_sector_size();

    printf("disk=0x%x sector_size=%u\n\r", disk_code, disk_sector_size);

    mbr_partition_t part = {0};
    u8 rootfs_index = 0;

    if (!_find_rootfs(&part, &rootfs_index)) {
        panic("rootfs partition not found");
    }

    rootfs_partition = part;
    rootfs_partition_valid = true;
    rootfs_partition_index = rootfs_index;

    if (part.lba_first > ((size_t)-1 / MBR_SECTOR_SIZE)) {
        panic("rootfs offset too large");
    }

    if (part.sector_count > ((size_t)-1 / MBR_SECTOR_SIZE)) {
        panic("rootfs partition too large");
    }

    rootfs_base = part.lba_first * MBR_SECTOR_SIZE;
    rootfs_size = part.sector_count * MBR_SECTOR_SIZE;

    // printf(
    //     "rootfs lba=%u base=0x%x size=%u\n\r",
    //     part.lba_first,
    //     (unsigned)rootfs_base,
    //     (unsigned)rootfs_size
    // );

    if (!boot_ext2_init(&rootfs, _read_rootfs_bytes, NULL, rootfs_size)) {
        panic("not an ext2 filesystem");
    }
}

bool bios_boot_root_hint(boot_root_hint_t *out) {
    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (!rootfs_partition_valid) {
        return false;
    }

    out->valid = 1;
    bool removable = (disk_flags & (1U << 2)) != 0;

    if (removable) {
        out->media = BOOT_MEDIA_USB;
        out->transport = BOOT_TRANSPORT_USB;
    } else {
        out->media = BOOT_MEDIA_DISK;
        out->transport = BOOT_TRANSPORT_ATA;
    }

    out->part_style = BOOT_PARTSTYLE_MBR;
    out->part_index = (u8)(rootfs_partition_index + 1);
    out->bios_drive = (u8)(disk_code & 0xffU);

    if (rootfs.superblock.signature == EXT2_SIGNATURE) {
        out->rootfs_uuid_valid = 1;
        memcpy(
            out->rootfs_uuid,
            rootfs.superblock.fs_id,
            sizeof(out->rootfs_uuid)
        );
    }

    return true;
}

bool stage_rootfs_image(u64 *paddr, u64 *size) {
    if (!paddr || !size || !rootfs_size) {
        return false;
    }

    size_t alloc_size = ALIGN(rootfs_size, 0x1000);

    if (alloc_size < rootfs_size) {
        return false;
    }

    void *image =
        mmap_alloc_top(alloc_size, E820_KERNEL, 0x1000, PROTECTED_MODE_TOP);

    read_disk(image, rootfs_base, rootfs_size);

    if (alloc_size > rootfs_size) {
        memset((u8 *)image + rootfs_size, 0, alloc_size - rootfs_size);
    }

    *paddr = (u64)(uintptr_t)image;
    *size = (u64)rootfs_size;

    return true;
}


static bool _read_rootfs_bytes(
    void *dest,
    size_t offset,
    size_t bytes,
    void *ctx
) {
    (void)ctx;
    read_disk(dest, rootfs_base + offset, bytes);
    return true;
}

void *read_rootfs(const char *path) {
    return boot_ext2_read_file(&rootfs, path, NULL);
}
