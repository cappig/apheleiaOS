#include "disk.h"

#include <base/macros.h>
#include <base/types.h>
#include <fs/ext2.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bios.h"
#include "tty.h"
#include "x86/mbr.h"
#include "x86/regs.h"

#define SECTOR_SIZE 512

#define MAX_BIOS_SECTORS_PER_CALL 64
#define BOUNCE_SECTORS            16

static u16 disk_code = 0;
static size_t rootfs_base = 0;
static ext2_superblock_t superblock = {0};

static bool _bios_read_lba(void* dest, size_t lba, u16 sectors) {
    if (!dest || !sectors)
        return false;

    dap_t dap = {
        .size = sizeof(dap_t),
        .sectors = sectors,
        .destination = (u32)(uintptr_t)dest,
        .lba = lba,
    };

    regs32_t r = {0};
    r.ah = 0x42;
    r.dl = disk_code;
    r.esi = (uintptr_t)&dap;

    bios_call(0x13, &r, &r);
    return !(r.flags & FLAG_CF);
}

int read_disk(void* dest, size_t offset, size_t bytes) {
    size_t lba = offset / SECTOR_SIZE;
    size_t sector_off = offset % SECTOR_SIZE;

    uint8_t bounce[SECTOR_SIZE * BOUNCE_SECTORS];
    uint8_t* out = dest;

    while (bytes > 0) {
        size_t bytes_window = bytes + sector_off;
        size_t sectors_window = DIV_ROUND_UP(bytes_window, SECTOR_SIZE);
        size_t sectors_cap = min((size_t)MAX_BIOS_SECTORS_PER_CALL, (size_t)BOUNCE_SECTORS);
        u16 sectors = (u16)min(sectors_window, sectors_cap);

        if (!_bios_read_lba(bounce, lba, sectors))
            panic("Disk read error!");

        size_t available = (size_t)sectors * SECTOR_SIZE - sector_off;
        size_t to_copy = min(bytes, available);

        memcpy(out, bounce + sector_off, to_copy);

        out += to_copy;
        bytes -= to_copy;
        lba += sectors;
        sector_off = 0; // offset is only relevant for the first chunk
    }

    return 0;
}

static bool _find_rootfs(mbr_partition_t* rootfs) {
    mbr_t mbr;

    // Read the mbr and find the ext2 partition
    read_disk(&mbr, 0, sizeof(mbr_t));

    if (mbr.signature != MBR_SIGNATURE)
        panic("MBR has invalid signature!");

    for (size_t i = 0; i < 4; i++) {
        mbr_partition_t* partition = &mbr.table.partitions[i];

        // We are looking for a ext2 partition
        if (partition->type != MBR_LINUX)
            continue;

        // ignore the bootable partition
        if (partition->status == MBR_BOOTABLE)
            continue;

        // We found our partition
        memcpy(rootfs, partition, sizeof(mbr_partition_t));

        return true;
    }

    return false;
}

void disk_init(u16 disk) {
    disk_code = disk;

    mbr_partition_t rootfs = {0};

    if (!_find_rootfs(&rootfs))
        panic("Rootfs partition not found!");

    rootfs_base = rootfs.lba_first * SECTOR_SIZE;

    // Read the superblock at offset 1024
    read_disk(&superblock, rootfs_base + 1024, sizeof(ext2_superblock_t));

    if (superblock.signature != EXT2_SIGNATURE)
        panic("Not an EXT2 filesystem!");

    // Verify filesystem state
    if (superblock.fs_state != EXT2_FS_CLEAN)
        panic("Filesystem has errors!");
}


static size_t _indirect_capacity(u32 entries_per_block, size_t indirection, size_t max) {
    size_t capacity = 1;

    for (size_t i = 0; i < indirection; i++) {
        if (capacity > max / entries_per_block)
            return max;

        capacity *= entries_per_block;
    }

    return capacity;
}

static void _push_zero_blocks(u32* blocks, size_t* n, size_t max, size_t count) {
    if (*n >= max)
        return;

    size_t remaining = max - *n;
    size_t to_add = (count < remaining) ? count : remaining;

    memset(&blocks[*n], 0, to_add * sizeof(u32));
    *n += to_add;
}

static void _flatten_blocks(u32* blocks, u32 block_num, size_t indirection, size_t* n, size_t max) {
    if (*n >= max)
        return;

    u32 block_size = ext2_block_size(&superblock);
    u32 entries_per_block = block_size / sizeof(u32);

    if (!indirection) {
        blocks[(*n)++] = block_num;
        return;
    }

    if (!block_num) {
        size_t capacity = _indirect_capacity(entries_per_block, indirection, max - *n);
        _push_zero_blocks(blocks, n, max, capacity);
        return;
    }

    u32* indirect_blocks = (u32*)malloc(block_size);

    if (!indirect_blocks)
        panic("Failed to allocate memory for indirect blocks!");

    read_disk(indirect_blocks, rootfs_base + (block_num * block_size), block_size);

    for (u32 i = 0; i < entries_per_block && *n < max; i++)
        _flatten_blocks(blocks, indirect_blocks[i], indirection - 1, n, max);

    free(indirect_blocks);
}


static void _get_inode(u32 num, ext2_inode_t* inode) {
    u32 block_size = ext2_block_size(&superblock);
    u32 group = (num - 1) / superblock.inodes_in_group;
    u32 index = (num - 1) % superblock.inodes_in_group;

    // Read the group descriptor
    size_t gdt_offset = rootfs_base + block_size * (superblock.superblock_offset + 1);
    size_t group_offset = gdt_offset + group * sizeof(ext2_group_descriptor_t);

    ext2_group_descriptor_t gd;
    read_disk(&gd, group_offset, sizeof(ext2_group_descriptor_t));

    // Calculate inode location
    u32 inode_size = ext2_inode_size(&superblock);
    u32 inode_offset = rootfs_base + (gd.inode_table_offset * block_size) + (index * inode_size);

    read_disk(inode, inode_offset, sizeof(ext2_inode_t));
}

static void* _read_inode(ext2_inode_t* inode) {
    u32 block_size = ext2_block_size(&superblock);

    // Flatten the inodes block list
    size_t inode_blocks = DIV_ROUND_UP(ext2_file_size(inode), block_size);

    u32* blocks = (u32*)malloc(inode_blocks * sizeof(u32));

    if (!blocks)
        panic("Failed to allocate memory for block list!");

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

    if (n != inode_blocks)
        panic("Inode block count mismatch!");

    void* buffer = malloc(inode_blocks * block_size);

    if (!buffer)
        panic("Failed to allocate memory for inode buffer!");

    for (size_t i = 0; i < inode_blocks; i++) {
        size_t block_offset = blocks[i] * block_size;
        void* out = buffer + (i * block_size);

        if (!blocks[i]) {
            memset(out, 0, block_size);
            continue;
        }

        read_disk(out, rootfs_base + block_offset, block_size);
    }

    free(blocks);

    return buffer;
}

static u32 _find_file(const char* path) {
    if (!path || path[0] != '/')
        return 0;

    u32 current_inode = EXT2_ROOT_INODE;
    const char* current = path + 1; // drop the leading '/'

    u32 block_size = ext2_block_size(&superblock);

    while (*current) {
        ext2_inode_t inode;
        _get_inode(current_inode, &inode);

        if (!ext2_is_type(&inode, EXT2_IT_DIR))
            return 0;

        size_t name_len = strnlend(current, '/', 255);

        void* inode_buffer = _read_inode(&inode);

        bool found = false;
        size_t offset = 0;

        while (offset < block_size) {
            ext2_directory_t* dir = (ext2_directory_t*)(inode_buffer + offset);

            if (!dir->inode || !dir->size)
                break;

            if (name_len == dir->name_size && !memcmp(dir->name, current, name_len)) {
                current_inode = dir->inode;
                found = true;

                break;
            }

            offset += dir->size;
        }

        free(inode_buffer);

        if (!found)
            return 0;

        current += name_len;
        if (*current == '/')
            current++;
    }

    return current_inode;
}


void* read_rootfs(const char* path) {
    u32 inode_num = _find_file(path);

    if (!inode_num)
        return NULL;

    ext2_inode_t inode;
    _get_inode(inode_num, &inode);

    return _read_inode(&inode);
}
