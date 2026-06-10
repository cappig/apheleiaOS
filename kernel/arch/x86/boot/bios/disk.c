#include "disk.h"

#include <base/macros.h>
#include <base/types.h>
#include <common/ext2.h>
#include <limits.h>
#include <log/log.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bios.h"
#include "memory.h"
#include "tty.h"
#include "x86/boot.h"
#include "x86/mbr.h"
#include "x86/regs.h"

#define MBR_SECTOR_SIZE 512

#define BIOS_MAX_SECTORS 64
#define BOUNCE_SIZE      8192

typedef struct {
    u16 drive;
    u16 sector_size;
    u16 flags;

    size_t root_base;
    size_t root_size;

    bool root_found;
    u8 root_part;

    boot_ext2_t fs;
    u8 bounce[BOUNCE_SIZE];
} bios_disk_t;

static bios_disk_t bios_disk = {
    .sector_size = MBR_SECTOR_SIZE,
};

static bool read_lba(void *dest, size_t lba, u16 sectors) {
    if (!dest || !sectors) {
        return false;
    }

    u32 real_dest = (u32)REAL_OFFSET(dest) | ((u32)REAL_SEG(dest) << 16);

    dap_t dap = {
        .size = sizeof(dap_t),
        .sectors = sectors,
        .destination = real_dest,
        .lba = lba,
    };

    regs32_t r = { 0 };
    r.ah = 0x42;
    r.dl = bios_disk.drive;
    r.esi = (uintptr_t)&dap;

    bios_call(0x13, &r, &r);
    return !(r.flags & FLAG_CF);
}

int read_disk(void *dest, size_t offset, size_t bytes) {
    size_t ss = bios_disk.sector_size;

    if (!ss) {
        panic("invalid disk sector size");
    }

    size_t lba = offset / ss;
    size_t sector_off = offset % ss;

    u16 bounce_sectors = BOUNCE_SIZE / ss;
    u16 max_sectors = (u16)min((size_t)BIOS_MAX_SECTORS, (size_t)bounce_sectors);

    uint8_t *out = dest;

    while (bytes > 0) {
        if (bytes > SIZE_MAX - sector_off) {
            panic("disk read size overflow");
        }

        size_t bytes_window = bytes + sector_off;
        size_t sectors_window = DIV_ROUND_UP(bytes_window, ss);
        u16 sectors = (u16)min(sectors_window, (size_t)max_sectors);

        if (!read_lba(bios_disk.bounce, lba, sectors)) {
            panic("disk read error");
        }

        size_t available = (size_t)sectors * ss - sector_off;
        size_t to_copy = min(bytes, available);

        memcpy(out, bios_disk.bounce + sector_off, to_copy);

        out += to_copy;
        bytes -= to_copy;
        lba += sectors;
        sector_off = 0;
    }

    return 0;
}

static bool read_rootfs_cb(void *dest, size_t offset, size_t bytes, void *ctx) {
    (void)ctx;

    if (offset > SIZE_MAX - bios_disk.root_base) {
        panic("rootfs read offset overflow");
    }

    read_disk(dest, bios_disk.root_base + offset, bytes);
    return true;
}

static bool find_part(mbr_partition_t *out_part, u8 *part_index) {
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

static void detect_sector(void) {
    disk_params_t params;
    memset(&params, 0, sizeof(params));
    params.size = sizeof(params);

    regs32_t r = { 0 };
    r.ah = 0x48;
    r.dl = bios_disk.drive;
    r.esi = (uintptr_t)&params;

    bios_call(0x13, &r, &r);

    if (!(r.flags & FLAG_CF)) {
        bios_disk.flags = params.flags;
        if (params.bytes_per_sector >= 512) {
            bios_disk.sector_size = params.bytes_per_sector;
        }
    }
}

void disk_init(u16 drive) {
    bios_disk.drive = drive;

    detect_sector();
    log_debug("boot disk=%#x sector=%u", bios_disk.drive, bios_disk.sector_size);

    mbr_partition_t part = { 0 };
    u8 rootfs_index = 0;

    if (!find_part(&part, &rootfs_index)) {
        panic("rootfs partition not found");
    }

    bios_disk.root_found = true;
    bios_disk.root_part = rootfs_index;

    if (part.lba_first > ((size_t)-1 / MBR_SECTOR_SIZE)) {
        panic("rootfs offset too large");
    }

    if (part.sector_count > ((size_t)-1 / MBR_SECTOR_SIZE)) {
        panic("rootfs partition too large");
    }

    bios_disk.root_base = part.lba_first * MBR_SECTOR_SIZE;
    bios_disk.root_size = part.sector_count * MBR_SECTOR_SIZE;

    log_debug(
        "rootfs partition lba=%u base=%#zx size=%zu",
        (unsigned int)part.lba_first,
        bios_disk.root_base,
        bios_disk.root_size
    );

    if (!boot_ext2_init(&bios_disk.fs, read_rootfs_cb, NULL, bios_disk.root_size)) {
        panic("not an ext2 filesystem");
    }
}

bool bios_boot_root_hint(boot_root_hint_t *out) {
    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (!bios_disk.root_found) {
        return false;
    }

    out->valid = 1;

    out->media = BOOT_MEDIA_DISK;
    out->transport = BOOT_TRANSPORT_ATA;
    out->part_style = BOOT_PARTSTYLE_MBR;
    out->part_index = (u8)(bios_disk.root_part + 1);
    out->bios_drive = (u8)(bios_disk.drive & 0xffU);

    if (bios_disk.fs.superblock.signature == EXT2_SIGNATURE) {
        out->rootfs_uuid_valid = 1;
        memcpy(out->rootfs_uuid, bios_disk.fs.superblock.fs_id, sizeof(out->rootfs_uuid));
    }

    return true;
}

bool stage_rootfs_image(u64 *paddr, u64 *size) {
    if (!paddr || !size || !bios_disk.root_size) {
        return false;
    }

    size_t alloc_size = ALIGN(bios_disk.root_size, 0x1000);

    if (alloc_size < bios_disk.root_size) {
        return false;
    }

    void *image = mmap_alloc_top(alloc_size, E820_KERNEL, 0x1000, PROTECTED_MODE_TOP);

    read_disk(image, bios_disk.root_base, bios_disk.root_size);

    if (alloc_size > bios_disk.root_size) {
        memset((u8 *)image + bios_disk.root_size, 0, alloc_size - bios_disk.root_size);
    }

    *paddr = (u64)(uintptr_t)image;
    *size = (u64)bios_disk.root_size;

    return true;
}

void *read_rootfs(const char *path) {
    return boot_ext2_read_file(&bios_disk.fs, path, NULL);
}
