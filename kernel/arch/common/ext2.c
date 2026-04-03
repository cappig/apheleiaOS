#include "ext2.h"

#include <base/macros.h>
#include <stdlib.h>
#include <string.h>

int printf(const char *fmt, ...);
void panic(const char *msg);

static size_t _strnlen_delim(const char *str, char delim, size_t max) {
    size_t len = 0;

    while (len < max && str[len] && str[len] != delim) {
        len++;
    }

    return len;
}

static void _read_fs(boot_ext2_t *fs, void *dest, size_t offset, size_t bytes) {
    if (!dest || !bytes) {
        return;
    }

    if (fs->size) {
        if (offset > fs->size || bytes > fs->size - offset) {
            panic("rootfs read out of bounds");
        }
    }

    if (!fs->read(dest, offset, bytes, fs->ctx)) {
        panic("rootfs read error");
    }
}

bool boot_ext2_init(
    boot_ext2_t *fs,
    boot_ext2_read_fn_t read,
    void *ctx,
    size_t size_limit
) {
    if (!fs || !read) {
        return false;
    }

    memset(fs, 0, sizeof(*fs));
    fs->read = read;
    fs->ctx = ctx;
    fs->size = size_limit;

    _read_fs(fs, &fs->superblock, 1024, sizeof(ext2_superblock_t));

    if (fs->superblock.signature != EXT2_SIGNATURE) {
        return false;
    }

    if (fs->superblock.fs_state != EXT2_FS_CLEAN) {
        panic("filesystem has errors");
    }

    u64 block_size = ext2_block_size(&fs->superblock);
    u64 blocks = fs->superblock.block_count;
    u64 size = block_size * blocks;

    if (!block_size || size > (u64)(size_t)-1) {
        panic("invalid rootfs size");
    }

    if (size_limit && size > size_limit) {
        panic("rootfs exceeds partition");
    }

    fs->size = (size_t)size;
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
    boot_ext2_t *fs,
    u32 *blocks,
    u32 block_num,
    size_t indirection,
    size_t *n,
    size_t max
) {
    if (*n >= max) {
        return;
    }

    u32 block_size = ext2_block_size(&fs->superblock);
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

    _read_fs(fs, indirect_blocks, block_num * block_size, block_size);

    for (u32 i = 0; i < entries_per_block && *n < max; i++) {
        _flatten_blocks(fs, blocks, indirect_blocks[i], indirection - 1, n, max);
    }

    free(indirect_blocks);
}

static void _get_inode(boot_ext2_t *fs, u32 num, ext2_inode_t *inode) {
    u32 block_size = ext2_block_size(&fs->superblock);
    u32 group = (num - 1) / fs->superblock.inodes_in_group;
    u32 index = (num - 1) % fs->superblock.inodes_in_group;

    size_t gdt_offset = block_size * (fs->superblock.superblock_offset + 1);
    size_t group_offset = gdt_offset + group * sizeof(ext2_group_descriptor_t);

    ext2_group_descriptor_t gd;
    _read_fs(fs, &gd, group_offset, sizeof(ext2_group_descriptor_t));

    u32 inode_size = ext2_inode_size(&fs->superblock);
    size_t inode_offset =
        (gd.inode_table_offset * block_size) + (index * inode_size);

    _read_fs(fs, inode, inode_offset, sizeof(ext2_inode_t));
}

static void *_read_inode(boot_ext2_t *fs, const ext2_inode_t *inode) {
    u32 block_size = ext2_block_size(&fs->superblock);
    u64 file_size_u64 = ext2_file_size(inode);

    if (file_size_u64 > (u64)(size_t)-1 - 1) {
        panic("file too large for bootloader");
    }

    size_t file_size = (size_t)file_size_u64;
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

    size_t direct_count = inode_blocks;
    if (direct_count > ARRAY_LEN(inode->direct_block_ptr)) {
        direct_count = ARRAY_LEN(inode->direct_block_ptr);
    }

    size_t n = 0;
    memcpy(blocks, inode->direct_block_ptr, direct_count * sizeof(u32));
    n = direct_count;

    _flatten_blocks(fs, blocks, inode->indirect_block_ptr[0], 1, &n, inode_blocks);
    _flatten_blocks(fs, blocks, inode->indirect_block_ptr[1], 2, &n, inode_blocks);
    _flatten_blocks(fs, blocks, inode->indirect_block_ptr[2], 3, &n, inode_blocks);

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
        void *out = (u8 *)buffer + (i * block_size);

        if (!blocks[i]) {
            memset(out, 0, block_size);
            continue;
        }

        _read_fs(fs, out, block_offset, block_size);
    }

    free(blocks);

    ((u8 *)buffer)[file_size] = '\0';
    return buffer;
}

static u32 _find_file(boot_ext2_t *fs, const char *path) {
    if (!path || path[0] != '/') {
        return 0;
    }

    u32 current_inode = EXT2_ROOT_INODE;
    const char *current = path + 1;

    while (*current) {
        ext2_inode_t inode;
        _get_inode(fs, current_inode, &inode);

        if (!ext2_is_type(&inode, EXT2_IT_DIR)) {
            return 0;
        }

        size_t name_len = _strnlen_delim(current, '/', 255);
        void *inode_buffer = _read_inode(fs, &inode);

        bool found = false;
        size_t offset = 0;
        u64 dir_size = ext2_file_size(&inode);

        while (offset < dir_size) {
            ext2_directory_t *dir =
                (ext2_directory_t *)((u8 *)inode_buffer + offset);

            if (!dir->inode || !dir->size) {
                break;
            }

            if (name_len == dir->name_size &&
                !memcmp(dir->name, current, name_len)) {
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

void *boot_ext2_read_file(
    boot_ext2_t *fs,
    const char *path,
    size_t *out_size
) {
    if (out_size) {
        *out_size = 0;
    }

    u32 inode_num = _find_file(fs, path);

    if (!inode_num) {
        return NULL;
    }

    ext2_inode_t inode;
    _get_inode(fs, inode_num, &inode);

    void *buffer = _read_inode(fs, &inode);

    if (out_size) {
        *out_size = (size_t)ext2_file_size(&inode);
    }

    return buffer;
}
