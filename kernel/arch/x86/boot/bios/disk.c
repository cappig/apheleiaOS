#include "disk.h"

#include <base/macros.h>
#include <base/types.h>
#include <fs/ext2.h>
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

static ext2_superblock_t superblock = {0};
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

static bool _find_rootfs(mbr_partition_t *rootfs, u8 *part_index) {
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

        memcpy(rootfs, partition, sizeof(mbr_partition_t));
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

    mbr_partition_t rootfs = {0};
    u8 rootfs_index = 0;

    if (!_find_rootfs(&rootfs, &rootfs_index)) {
        panic("rootfs partition not found");
    }

    rootfs_partition = rootfs;
    rootfs_partition_valid = true;
    rootfs_partition_index = rootfs_index;

    if (rootfs.lba_first > ((size_t)-1 / MBR_SECTOR_SIZE)) {
        panic("rootfs offset too large");
    }

    if (rootfs.sector_count > ((size_t)-1 / MBR_SECTOR_SIZE)) {
        panic("rootfs partition too large");
    }

    rootfs_base = rootfs.lba_first * MBR_SECTOR_SIZE;
    rootfs_size = rootfs.sector_count * MBR_SECTOR_SIZE;

    // printf(
    //     "rootfs lba=%u base=0x%x size=%u\n\r",
    //     rootfs.lba_first,
    //     (unsigned)rootfs_base,
    //     (unsigned)rootfs_size
    // );

    read_disk(&superblock, rootfs_base + 1024, sizeof(ext2_superblock_t));

    if (superblock.signature != EXT2_SIGNATURE) {
        panic("not an ext2 filesystem");
    }

    if (superblock.fs_state != EXT2_FS_CLEAN) {
        panic("filesystem has errors");
    }

    // printf(
    //     "ext2 blocks=%u block_size=%u\n\r",
    //     superblock.block_count,
    //     ext2_block_size(&superblock)
    // );
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

    if (superblock.signature == EXT2_SIGNATURE) {
        out->rootfs_uuid_valid = 1;
        memcpy(out->rootfs_uuid, superblock.fs_id, sizeof(out->rootfs_uuid));
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


static size_t
_indirect_capacity(u32 entries_per_block, size_t indirection, size_t max) {
    size_t capacity = 1;

    for (size_t i = 0; i < indirection; i++) {
        if (capacity > max / entries_per_block) {
            return max;
        }

        capacity *= entries_per_block;
    }

    return capacity;
}

static void
_push_zero_blocks(u32 *blocks, size_t *n, size_t max, size_t count) {
    if (*n >= max) {
        return;
    }

    size_t remaining = max - *n;
    size_t to_add = (count < remaining) ? count : remaining;

    memset(&blocks[*n], 0, to_add * sizeof(u32));
    *n += to_add;
}

static void _flatten_blocks(
    u32 *blocks,
    u32 block_num,
    size_t indirection,
    size_t *n,
    size_t max
) {
    if (*n >= max) {
        return;
    }

    u32 block_size = ext2_block_size(&superblock);
    u32 entries_per_block = block_size / sizeof(u32);

    if (!indirection) {
        blocks[(*n)++] = block_num;
        return;
    }

    if (!block_num) {
        size_t capacity =
            _indirect_capacity(entries_per_block, indirection, max - *n);
        _push_zero_blocks(blocks, n, max, capacity);
        return;
    }

    u32 *indirect_blocks = (u32 *)malloc(block_size);

    if (!indirect_blocks) {
        panic("failed to allocate memory for indirect blocks");
    }

    read_disk(
        indirect_blocks, rootfs_base + (block_num * block_size), block_size
    );

    for (u32 i = 0; i < entries_per_block && *n < max; i++) {
        _flatten_blocks(blocks, indirect_blocks[i], indirection - 1, n, max);
    }

    free(indirect_blocks);
}


static void _get_inode(u32 num, ext2_inode_t *inode) {
    u32 block_size = ext2_block_size(&superblock);
    u32 group = (num - 1) / superblock.inodes_in_group;
    u32 index = (num - 1) % superblock.inodes_in_group;

    // Read the group descriptor
    size_t gdt_offset =
        rootfs_base + block_size * (superblock.superblock_offset + 1);
    size_t group_offset = gdt_offset + group * sizeof(ext2_group_descriptor_t);

    ext2_group_descriptor_t gd;
    read_disk(&gd, group_offset, sizeof(ext2_group_descriptor_t));

    // Calculate inode location
    u32 inode_size = ext2_inode_size(&superblock);
    u32 inode_offset = 
        rootfs_base + (gd.inode_table_offset * block_size) + (index * inode_size);

    read_disk(inode, inode_offset, sizeof(ext2_inode_t));
}

static void *_read_inode(ext2_inode_t *inode) {
    u32 block_size = ext2_block_size(&superblock);
    u64 file_size_u64 = ext2_file_size(inode);

    if (file_size_u64 > (u64)(size_t)-1 - 1) {
        panic("file too large for bootloader");
    }

    size_t file_size = (size_t)file_size_u64;

    // Flatten the inodes block list
    size_t inode_blocks = DIV_ROUND_UP(file_size, block_size);

    if (!inode_blocks) {
        char *buffer = malloc(1);

        if (!buffer) {
            panic("failed to allocate memory for inode buffer");
        }

        buffer[0] = '\0';
        return buffer;
    }

    u32 *blocks = (u32 *)malloc(inode_blocks * sizeof(u32));

    if (!blocks) {
        panic("failed to allocate memory for block list");
    }

    size_t n = 0;

    // Direct blocks
    int direct_count = min(12, inode_blocks);
    memcpy(blocks, inode->direct_block_ptr, direct_count * sizeof(u32));
    n = direct_count;

    // Singly indirect block
    _flatten_blocks(blocks, inode->indirect_block_ptr[0], 1, &n, inode_blocks);

    // Doubly indirect block
    _flatten_blocks(blocks, inode->indirect_block_ptr[1], 2, &n, inode_blocks);

    // Triply indirect block
    _flatten_blocks(blocks, inode->indirect_block_ptr[2], 3, &n, inode_blocks);

    if (n != inode_blocks) {
        panic("inode block count mismatch");
    }

    size_t buffer_size = inode_blocks * block_size;
    void *buffer = malloc(buffer_size + 1);

    if (!buffer) {
        panic("failed to allocate memory for inode buffer");
    }

    for (size_t i = 0; i < inode_blocks; i++) {
        size_t block_offset = blocks[i] * block_size;
        void *out = buffer + (i * block_size);

        if (!blocks[i]) {
            memset(out, 0, block_size);
            continue;
        }

        read_disk(out, rootfs_base + block_offset, block_size);
    }

    free(blocks);

    u8 *bytes = (u8 *)buffer;
    bytes[file_size] = '\0';

    return buffer;
}

static u32 _find_file(const char *path) {
    if (!path || path[0] != '/') {
        return 0;
    }

    u32 current_inode = EXT2_ROOT_INODE;
    const char *current = path + 1; // drop the leading '/'

    while (*current) {
        ext2_inode_t inode;
        _get_inode(current_inode, &inode);

        if (!ext2_is_type(&inode, EXT2_IT_DIR)) {
            return 0;
        }

        size_t name_len = strnlend(current, '/', 255);

        void *inode_buffer = _read_inode(&inode);

        bool found = false;
        size_t offset = 0;
        u64 dir_size = ext2_file_size(&inode);

        while (offset < dir_size) {
            ext2_directory_t *dir = (ext2_directory_t *)(inode_buffer + offset);

            if (!dir->inode || !dir->size) {
                break;
            }

            if (name_len == dir->name_size && !memcmp(dir->name, current, name_len)) {
                current_inode = dir->inode;
                found = true;

                break;
            }

            offset += dir->size;
        }

        free(inode_buffer);

        if (!found) {
            printf("'%s' not found\n\r", path);
            return 0;
        }

        current += name_len;
        if (*current == '/') {
            current++;
        }
    }

    return current_inode;
}


void *read_rootfs(const char *path) {
    u32 inode_num = _find_file(path);

    if (!inode_num) {
        return NULL;
    }

    ext2_inode_t inode;
    _get_inode(inode_num, &inode);

    return _read_inode(&inode);
}
