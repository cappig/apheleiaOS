#include "ext2fs.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <data/bitmap.h>
#include <errno.h>
#include <fs/ext2.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>

#include "sys/disk.h"
#include "sys/vfs.h"

#define EXT2_BLOCK_CACHE_SIZE 64

#define EXT2_ATIME_NOATIME  0
#define EXT2_ATIME_RELATIME 1
#define EXT2_ATIME_STRICT   2

#ifndef EXT2_ATIME_POLICY
#define EXT2_ATIME_POLICY EXT2_ATIME_NOATIME
#endif

#define EXT2_RELATIME_WINDOW_SECS (24U * 60U * 60U)

typedef struct {
    bool valid;
    u32 block;
    u64 stamp;
    u8 *data;
} ext2_block_cache_entry_t;

typedef struct {
    ext2_superblock_t superblock;
    ext2_group_descriptor_t *groups;
    u32 group_count;
    u32 block_size;
    u32 inode_size;
    u32 time_base;
    u64 time_tick_base;
    size_t gdt_offset;
    size_t gdt_size;
    ext2_block_cache_entry_t cache[EXT2_BLOCK_CACHE_SIZE];
    u8 *cache_data;
    u64 cache_clock;
} ext2_private_t;

typedef struct {
    u32 inode_num;
    ext2_inode_t inode;
} ext2_node_info_t;


static bool
_ext2_read(disk_partition_t *part, void *dest, size_t offset, size_t bytes) {
    if (!part || !part->disk || !part->disk->interface || !part->disk->interface->read) {
        return false;
    }

    ssize_t read = part->disk->interface->read(
        part->disk, dest, part->offset + offset, bytes
    );

    return read == (ssize_t)bytes;
}

static bool _ext2_write(
    disk_partition_t *part,
    const void *src,
    size_t offset,
    size_t bytes
) {
    if (!part || !part->disk || !part->disk->interface || !part->disk->interface->write) {
        return false;
    }

    ssize_t written = part->disk->interface->write(
        part->disk, (void *)src, part->offset + offset, bytes
    );

    return written == (ssize_t)bytes;
}

static ext2_block_cache_entry_t *_cache_find(ext2_private_t *priv, u32 block) {
    if (!priv || !priv->cache_data) {
        return NULL;
    }

    for (size_t i = 0; i < EXT2_BLOCK_CACHE_SIZE; i++) {
        ext2_block_cache_entry_t *entry = &priv->cache[i];

        if (entry->valid && entry->block == block) {
            return entry;
        }
    }

    return NULL;
}

static ext2_block_cache_entry_t *_cache_slot(ext2_private_t *priv) {
    if (!priv || !priv->cache_data) {
        return NULL;
    }

    ext2_block_cache_entry_t *victim = NULL;

    for (size_t i = 0; i < EXT2_BLOCK_CACHE_SIZE; i++) {
        ext2_block_cache_entry_t *entry = &priv->cache[i];

        if (!entry->data) {
            continue;
        }

        if (!entry->valid) {
            return entry;
        }

        if (!victim || entry->stamp < victim->stamp) {
            victim = entry;
        }
    }

    return victim;
}

static void _cache_store(ext2_private_t *priv, u32 block, const void *data) {
    if (!priv || !priv->cache_data || !data) {
        return;
    }

    ext2_block_cache_entry_t *entry = _cache_find(priv, block);
    if (!entry) {
        entry = _cache_slot(priv);
    }

    if (!entry || !entry->data) {
        return;
    }

    memcpy(entry->data, data, priv->block_size);
    entry->valid = true;
    entry->block = block;
    entry->stamp = ++priv->cache_clock;
}

static bool _read_block(
    ext2_private_t *priv,
    disk_partition_t *part,
    u32 block,
    void *dest
) {
    if (!priv || !part || !dest) {
        return false;
    }

    if (!block) {
        memset(dest, 0, priv->block_size);
        return true;
    }

    ext2_block_cache_entry_t *cached = _cache_find(priv, block);
    if (cached && cached->data) {
        memcpy(dest, cached->data, priv->block_size);
        cached->stamp = ++priv->cache_clock;
        return true;
    }

    size_t offset = (size_t)block * priv->block_size;
    if (!_ext2_read(part, dest, offset, priv->block_size)) {
        return false;
    }

    _cache_store(priv, block, dest);
    return true;
}

static bool _write_block(
    ext2_private_t *priv,
    disk_partition_t *part,
    u32 block,
    const void *src
) {
    if (!priv || !part || !src || !block) {
        return false;
    }

    size_t offset = (size_t)block * priv->block_size;
    if (!_ext2_write(part, src, offset, priv->block_size)) {
        return false;
    }

    _cache_store(priv, block, src);
    return true;
}

static bool _read_inode(
    ext2_private_t *priv,
    disk_partition_t *part,
    u32 inode_num,
    ext2_inode_t *inode
) {
    if (!priv || !part || !inode || !inode_num) {
        return false;
    }

    u32 group = (inode_num - 1) / priv->superblock.inodes_in_group;
    u32 index = (inode_num - 1) % priv->superblock.inodes_in_group;

    if (group >= priv->group_count) {
        return false;
    }

    ext2_group_descriptor_t *gd = &priv->groups[group];

    size_t inode_table = (size_t)gd->inode_table_offset * priv->block_size;
    size_t inode_offset = inode_table + (size_t)index * priv->inode_size;

    memset(inode, 0, sizeof(ext2_inode_t));

    size_t read_size = priv->inode_size;

    if (read_size > sizeof(ext2_inode_t)) {
        read_size = sizeof(ext2_inode_t);
    }

    return _ext2_read(part, inode, inode_offset, read_size);
}

static bool _write_inode(
    ext2_private_t *priv,
    disk_partition_t *part,
    u32 inode_num,
    const ext2_inode_t *inode
) {
    if (!priv || !part || !inode || !inode_num) {
        return false;
    }

    u32 group = (inode_num - 1) / priv->superblock.inodes_in_group;
    u32 index = (inode_num - 1) % priv->superblock.inodes_in_group;

    if (group >= priv->group_count) {
        return false;
    }

    ext2_group_descriptor_t *gd = &priv->groups[group];

    size_t inode_table = (size_t)gd->inode_table_offset * priv->block_size;
    size_t inode_offset = inode_table + (size_t)index * priv->inode_size;

    size_t write_size = priv->inode_size;

    if (write_size > sizeof(ext2_inode_t)) {
        write_size = sizeof(ext2_inode_t);
    }

    return _ext2_write(part, inode, inode_offset, write_size);
}

static bool
_clear_inode(ext2_private_t *priv, disk_partition_t *part, u32 inode_num) {
    ext2_inode_t zero = {0};
    return _write_inode(priv, part, inode_num, &zero);
}

static bool _write_super(ext2_private_t *priv, disk_partition_t *part) {
    if (!priv || !part) {
        return false;
    }

    return _ext2_write(
        part, &priv->superblock, 1024, sizeof(ext2_superblock_t)
    );
}

static bool _write_groups(ext2_private_t *priv, disk_partition_t *part) {
    if (!priv || !part || !priv->groups) {
        return false;
    }

    return _ext2_write(part, priv->groups, priv->gdt_offset, priv->gdt_size);
}

static bool
_flush_alloc_metadata(ext2_private_t *priv, disk_partition_t *part) {
    return _write_super(priv, part) && _write_groups(priv, part);
}

static bool _group_block_bitmap(
    ext2_private_t *priv,
    disk_partition_t *part,
    u32 group,
    u8 *bitmap
) {
    if (!priv || !part || !bitmap || group >= priv->group_count) {
        return false;
    }

    u32 bitmap_block = priv->groups[group].usage_bitmap_offset;

    return _read_block(priv, part, bitmap_block, bitmap);
}

static bool _write_group_block_bitmap(
    ext2_private_t *priv,
    disk_partition_t *part,
    u32 group,
    const u8 *bitmap
) {
    if (!priv || !part || !bitmap || group >= priv->group_count) {
        return false;
    }

    u32 bitmap_block = priv->groups[group].usage_bitmap_offset;

    return _write_block(priv, part, bitmap_block, bitmap);
}

static bool _group_inode_bitmap(
    ext2_private_t *priv,
    disk_partition_t *part,
    u32 group,
    u8 *bitmap
) {
    if (!priv || !part || !bitmap || group >= priv->group_count) {
        return false;
    }

    u32 bitmap_block = priv->groups[group].inode_bitmap_offset;

    return _read_block(priv, part, bitmap_block, bitmap);
}

static bool _write_group_inode_bitmap(
    ext2_private_t *priv,
    disk_partition_t *part,
    u32 group,
    const u8 *bitmap
) {
    if (!priv || !part || !bitmap || group >= priv->group_count) {
        return false;
    }

    u32 bitmap_block = priv->groups[group].inode_bitmap_offset;

    return _write_block(priv, part, bitmap_block, bitmap);
}

static bool
_find_free_bit(const u8 *bitmap, size_t bit_count, size_t *index_out) {
    if (!bitmap || !index_out) {
        return false;
    }

    for (size_t i = 0; i < bit_count; i++) {
        size_t byte = i / 8;
        size_t bit = i % 8;

        if (!(bitmap[byte] & (1u << bit))) {
            *index_out = i;
            return true;
        }
    }

    return false;
}

static void _set_file_size(ext2_inode_t *inode, u64 size) {
    if (!inode) {
        return;
    }

    inode->size_low = (u32)(size & 0xffffffffu);
    inode->size_high = (u32)(size >> 32);
}

static u32 _now(ext2_private_t *priv) {
    if (!priv) {
        return 0;
    }

    u32 hz = arch_timer_hz();
    if (!hz) {
        return priv->time_base;
    }

    u64 now_ticks = arch_timer_ticks();
    u64 delta_ticks = now_ticks - priv->time_tick_base;
    u64 delta_secs = delta_ticks / hz;

    return priv->time_base + (u32)delta_secs;
}

static bool _should_update_atime(const ext2_inode_t *inode, u32 now) {
    if (!inode) {
        return false;
    }

#if EXT2_ATIME_POLICY == EXT2_ATIME_STRICT
    (void)now;
    return true;
#elif EXT2_ATIME_POLICY == EXT2_ATIME_RELATIME
    if (inode->last_access_time <= inode->last_modification_time) {
        return true;
    }

    if (inode->last_access_time <= inode->creation_time) {
        return true;
    }

    return now - inode->last_access_time >= EXT2_RELATIME_WINDOW_SECS;
#else
    (void)now;
    return false;
#endif
}

static bool _update_super_write_time(
    ext2_private_t *priv,
    disk_partition_t *part,
    u32 now
) {
    if (!priv || !part) {
        return false;
    }

    priv->superblock.last_write_time = now;
    return _write_super(priv, part);
}

static void _sync_vnode(vfs_node_t *node, const ext2_inode_t *inode) {
    if (!node || !inode) {
        return;
    }

    node->size = ext2_file_size(inode);
    node->time.accessed = inode->last_access_time;
    node->time.created = inode->creation_time;
    node->time.modified = inode->last_modification_time;
}

static bool
_alloc_block(ext2_private_t *priv, disk_partition_t *part, u32 *out_block) {
    if (!priv || !part || !out_block) {
        return false;
    }

    u8 *bitmap = malloc(priv->block_size);
    if (!bitmap) {
        return false;
    }

    bool ok = false;

    for (u32 group = 0; group < priv->group_count; group++) {
        ext2_group_descriptor_t *gd = &priv->groups[group];
        if (!gd->unallocated_block_count) {
            continue;
        }

        if (!_group_block_bitmap(priv, part, group, bitmap)) {
            break;
        }

        u32 first = group * priv->superblock.blocks_in_group;
        size_t count = priv->superblock.blocks_in_group;

        if (first >= priv->superblock.block_count) {
            continue;
        }

        if (first + count > priv->superblock.block_count) {
            count = priv->superblock.block_count - first;
        }

        size_t bit = 0;
        if (!_find_free_bit(bitmap, count, &bit)) {
            continue;
        }

        bitmap_set((bitmap_word_t *)bitmap, bit);

        if (!_write_group_block_bitmap(priv, part, group, bitmap)) {
            break;
        }

        gd->unallocated_block_count--;

        if (priv->superblock.free_block_count) {
            priv->superblock.free_block_count--;
        }

        if (!_flush_alloc_metadata(priv, part)) {
            break;
        }

        *out_block = first + (u32)bit;
        if (!*out_block) {
            continue;
        }
        ok = true;
        break;
    }

    free(bitmap);
    return ok;
}

static bool
_free_block(ext2_private_t *priv, disk_partition_t *part, u32 block) {
    if (!priv || !part || !block) {
        return false;
    }

    if (block >= priv->superblock.block_count) {
        return false;
    }

    u32 group = block / priv->superblock.blocks_in_group;
    u32 index = block % priv->superblock.blocks_in_group;

    if (group >= priv->group_count) {
        return false;
    }

    u8 *bitmap = malloc(priv->block_size);
    if (!bitmap) {
        return false;
    }

    bool ok = false;

    if (!_group_block_bitmap(priv, part, group, bitmap)) {
        goto out;
    }

    if (!bitmap_get((bitmap_word_t *)bitmap, index)) {
        ok = true;
        goto out;
    }

    bitmap_clear((bitmap_word_t *)bitmap, index);

    if (!_write_group_block_bitmap(priv, part, group, bitmap)) {
        goto out;
    }

    priv->groups[group].unallocated_block_count++;
    priv->superblock.free_block_count++;

    if (!_flush_alloc_metadata(priv, part)) {
        goto out;
    }

    ok = true;

out:
    free(bitmap);
    return ok;
}

static bool
_alloc_inode(ext2_private_t *priv, disk_partition_t *part, u32 *out_inode) {
    if (!priv || !part || !out_inode) {
        return false;
    }

    u8 *bitmap = malloc(priv->block_size);
    if (!bitmap) {
        return false;
    }

    bool ok = false;

    for (u32 group = 0; group < priv->group_count; group++) {
        ext2_group_descriptor_t *gd = &priv->groups[group];
        if (!gd->unallocated_inode_count) {
            continue;
        }

        if (!_group_inode_bitmap(priv, part, group, bitmap)) {
            break;
        }

        u32 first = group * priv->superblock.inodes_in_group;
        size_t count = priv->superblock.inodes_in_group;

        if (first >= priv->superblock.inode_count) {
            continue;
        }

        if (first + count > priv->superblock.inode_count) {
            count = priv->superblock.inode_count - first;
        }

        size_t bit = 0;
        if (!_find_free_bit(bitmap, count, &bit)) {
            continue;
        }

        bitmap_set((bitmap_word_t *)bitmap, bit);

        if (!_write_group_inode_bitmap(priv, part, group, bitmap)) {
            break;
        }

        gd->unallocated_inode_count--;

        if (priv->superblock.free_inode_count) {
            priv->superblock.free_inode_count--;
        }

        if (!_flush_alloc_metadata(priv, part)) {
            break;
        }

        *out_inode = first + (u32)bit + 1;
        ok = true;
        break;
    }

    free(bitmap);
    return ok;
}

static bool
_free_inode(ext2_private_t *priv, disk_partition_t *part, u32 inode_num) {
    if (!priv || !part || !inode_num) {
        return false;
    }

    u32 zero = inode_num - 1;
    if (zero >= priv->superblock.inode_count) {
        return false;
    }

    u32 group = zero / priv->superblock.inodes_in_group;
    u32 index = zero % priv->superblock.inodes_in_group;

    if (group >= priv->group_count) {
        return false;
    }

    u8 *bitmap = malloc(priv->block_size);
    if (!bitmap) {
        return false;
    }

    bool ok = false;

    if (!_group_inode_bitmap(priv, part, group, bitmap)) {
        goto out;
    }

    if (!bitmap_get((bitmap_word_t *)bitmap, index)) {
        ok = true;
        goto out;
    }

    bitmap_clear((bitmap_word_t *)bitmap, index);

    if (!_write_group_inode_bitmap(priv, part, group, bitmap)) {
        goto out;
    }

    priv->groups[group].unallocated_inode_count++;
    priv->superblock.free_inode_count++;

    if (!_flush_alloc_metadata(priv, part)) {
        goto out;
    }

    ok = true;

out:
    free(bitmap);
    return ok;
}

static bool
_zero_block(ext2_private_t *priv, disk_partition_t *part, u32 block) {
    if (!priv || !part || !block) {
        return false;
    }

    u8 *zero = calloc(1, priv->block_size);
    if (!zero) {
        return false;
    }

    bool ok = _write_block(priv, part, block, zero);

    free(zero);

    return ok;
}

static u32 _block_for_index(
    ext2_private_t *priv,
    disk_partition_t *part,
    const ext2_inode_t *inode,
    u32 block_index
) {
    if (!priv || !inode) {
        return 0;
    }

    if (block_index < 12) {
        return inode->direct_block_ptr[block_index];
    }

    block_index -= 12;

    u32 entries_per_block = priv->block_size / sizeof(u32);
    if (block_index >= entries_per_block) {
        return 0;
    }

    u32 indirect_block = inode->indirect_block_ptr[0];
    if (!indirect_block) {
        return 0;
    }

    u32 *table = malloc(priv->block_size);
    if (!table) {
        return 0;
    }

    if (!_read_block(priv, part, indirect_block, table)) {
        free(table);
        return 0;
    }

    u32 block = table[block_index];

    free(table);

    return block;
}

static bool _ensure_block(
    ext2_private_t *priv,
    disk_partition_t *part,
    ext2_node_info_t *info,
    u32 block_index,
    u32 *out_block,
    bool *out_changed
) {
    if (!priv || !part || !info || !out_block) {
        return false;
    }

    bool changed = false;
    u32 sectors_per_block = priv->block_size / 512;

    if (block_index < 12) {
        if (!info->inode.direct_block_ptr[block_index]) {
            u32 block = 0;

            if (!_alloc_block(priv, part, &block)) {
                return false;
            }

            if (!_zero_block(priv, part, block)) {
                _free_block(priv, part, block);
                return false;
            }

            info->inode.direct_block_ptr[block_index] = block;
            info->inode.disk_sector_count += sectors_per_block;
            changed = true;
        }

        *out_block = info->inode.direct_block_ptr[block_index];

        if (out_changed) {
            *out_changed = changed;
        }

        return true;
    }

    u32 index = block_index - 12;
    u32 entries_per_block = priv->block_size / sizeof(u32);

    if (index >= entries_per_block) {
        return false;
    }

    if (!info->inode.indirect_block_ptr[0]) {
        u32 ind_block = 0;

        if (!_alloc_block(priv, part, &ind_block)) {
            return false;
        }

        if (!_zero_block(priv, part, ind_block)) {
            _free_block(priv, part, ind_block);
            return false;
        }

        info->inode.indirect_block_ptr[0] = ind_block;
        info->inode.disk_sector_count += sectors_per_block;
        changed = true;
    }

    u32 *table = malloc(priv->block_size);
    if (!table) {
        return false;
    }

    bool ok = false;

    if (!_read_block(priv, part, info->inode.indirect_block_ptr[0], table)) {
        goto out;
    }

    if (!table[index]) {
        u32 block = 0;
        if (!_alloc_block(priv, part, &block)) {
            goto out;
        }

        if (!_zero_block(priv, part, block)) {
            _free_block(priv, part, block);
            goto out;
        }

        table[index] = block;
        bool wrote_indirect = _write_block(
            priv,
            part,
            info->inode.indirect_block_ptr[0],
            table
        );

        if (!wrote_indirect) {
            _free_block(priv, part, block);
            table[index] = 0;
            goto out;
        }

        info->inode.disk_sector_count += sectors_per_block;
        changed = true;
    }

    *out_block = table[index];

    if (out_changed) {
        *out_changed = changed;
    }

    ok = true;

out:
    free(table);
    return ok;
}

static bool _release_inode_blocks(
    ext2_private_t *priv,
    disk_partition_t *part,
    ext2_inode_t *inode
) {
    if (!priv || !part || !inode) {
        return false;
    }

    for (size_t i = 0; i < 12; i++) {
        if (!inode->direct_block_ptr[i]) {
            continue;
        }

        if (!_free_block(priv, part, inode->direct_block_ptr[i])) {
            return false;
        }

        inode->direct_block_ptr[i] = 0;
    }

    if (inode->indirect_block_ptr[0]) {
        u32 *table = malloc(priv->block_size);
        if (!table) {
            return false;
        }

        if (!_read_block(priv, part, inode->indirect_block_ptr[0], table)) {
            free(table);
            return false;
        }

        u32 entries_per_block = priv->block_size / sizeof(u32);

        for (u32 i = 0; i < entries_per_block; i++) {
            if (!table[i]) {
                continue;
            }

            if (!_free_block(priv, part, table[i])) {
                free(table);
                return false;
            }

            table[i] = 0;
        }

        free(table);

        if (!_free_block(priv, part, inode->indirect_block_ptr[0])) {
            return false;
        }

        inode->indirect_block_ptr[0] = 0;
    }

    inode->disk_sector_count = 0;
    _set_file_size(inode, 0);
    return true;
}

static u16 _vfs_to_inode_type(u32 vfs_type) {
    switch (vfs_type) {
    case VFS_DIR:
        return EXT2_IT_DIR;
    case VFS_FILE:
        return EXT2_IT_FILE;
    case VFS_CHARDEV:
        return EXT2_IT_CHAR_DEV;
    case VFS_BLOCKDEV:
        return EXT2_IT_BLOCK_DEV;
    default:
        return 0;
    }
}

static u8 _vfs_to_dir_type(u32 vfs_type) {
    switch (vfs_type) {
    case VFS_DIR:
        return EXT2_DIR_DIRECTORY;
    case VFS_FILE:
        return EXT2_DIR_REGULAR;
    case VFS_CHARDEV:
        return EXT2_DIR_CHAR_DEV;
    case VFS_BLOCKDEV:
        return EXT2_DIR_BLOCK_DEV;
    default:
        return EXT2_DIR_UNKNOWN;
    }
}

static u32 _inode_type_to_vfs(const ext2_inode_t *inode) {
    if (!inode) {
        return VFS_FILE;
    }

    if (ext2_is_type(inode, EXT2_IT_DIR)) {
        return VFS_DIR;
    }

    if (ext2_is_type(inode, EXT2_IT_FILE)) {
        return VFS_FILE;
    }

    if (ext2_is_type(inode, EXT2_IT_CHAR_DEV)) {
        return VFS_CHARDEV;
    }

    if (ext2_is_type(inode, EXT2_IT_BLOCK_DEV)) {
        return VFS_BLOCKDEV;
    }

    return VFS_FILE;
}

static size_t _dir_entry_size(size_t name_len) {
    return ALIGN(8 + name_len, 4);
}

static bool _dir_write_dots(
    ext2_private_t *priv,
    disk_partition_t *part,
    u32 block,
    u32 self_inode,
    u32 parent_inode
) {
    if (!priv || !part || !block) {
        return false;
    }

    u8 *buf = calloc(1, priv->block_size);
    if (!buf) {
        return false;
    }

    ext2_directory_t *dot = (ext2_directory_t *)buf;

    dot->inode = self_inode;
    dot->size = (u16)_dir_entry_size(1);
    dot->name_size = 1;
    dot->type = EXT2_DIR_DIRECTORY;
    dot->name[0] = '.';

    ext2_directory_t *dotdot = (ext2_directory_t *)(buf + dot->size);

    dotdot->inode = parent_inode;
    dotdot->size = (u16)(priv->block_size - dot->size);
    dotdot->name_size = 2;
    dotdot->type = EXT2_DIR_DIRECTORY;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    bool ok = _write_block(priv, part, block, buf);
    free(buf);
    return ok;
}

static bool _dir_find_entry(
    ext2_private_t *priv,
    disk_partition_t *part,
    const ext2_node_info_t *dir_info,
    const char *name,
    u32 *inode_out,
    u32 *block_out,
    size_t *pos_out,
    u16 *size_out,
    u8 *type_out
) {
    if (!priv || !part || !dir_info || !name || !name[0]) {
        return false;
    }

    u64 size = ext2_file_size(&dir_info->inode);
    if (!size) {
        return false;
    }

    u32 block_size = priv->block_size;
    u32 blocks = DIV_ROUND_UP(size, block_size);
    size_t wanted_len = strlen(name);

    u8 *block = malloc(block_size);
    if (!block) {
        return false;
    }

    bool found = false;

    for (u32 i = 0; i < blocks; i++) {
        u32 block_num = _block_for_index(priv, part, &dir_info->inode, i);
        if (!block_num) {
            continue;
        }

        if (!_read_block(priv, part, block_num, block)) {
            break;
        }

        size_t pos = 0;
        while (pos + 8 <= block_size) {
            ext2_directory_t *entry = (ext2_directory_t *)(block + pos);

            if (entry->size < 8 || entry->size > block_size - pos) {
                break;
            }

            if (
                entry->inode &&
                entry->name_size == wanted_len &&
                !memcmp(entry->name, name, wanted_len)
            ) {

                if (inode_out) {
                    *inode_out = entry->inode;
                }

                if (block_out) {
                    *block_out = block_num;
                }

                if (pos_out) {
                    *pos_out = pos;
                }

                if (size_out) {
                    *size_out = entry->size;
                }

                if (type_out) {
                    *type_out = entry->type;
                }

                found = true;
                break;
            }

            pos += entry->size;
        }

        if (found) {
            break;
        }
    }

    free(block);
    return found;
}

static bool _dir_add_entry(
    ext2_private_t *priv,
    disk_partition_t *part,
    ext2_node_info_t *dir_info,
    const char *name,
    u32 inode_num,
    u8 type
) {
    if (!priv || !part || !dir_info || !name || !name[0] || !inode_num) {
        return false;
    }

    size_t name_len = strlen(name);
    if (name_len > 255) {
        return false;
    }

    size_t need = _dir_entry_size(name_len);
    u32 block_size = priv->block_size;

    u64 size = ext2_file_size(&dir_info->inode);
    u32 blocks = DIV_ROUND_UP(size, block_size);

    u8 *block = malloc(block_size);
    if (!block) {
        return false;
    }

    for (u32 i = 0; i < blocks; i++) {
        u32 block_num = _block_for_index(priv, part, &dir_info->inode, i);
        if (!block_num) {
            continue;
        }

        if (!_read_block(priv, part, block_num, block)) {
            free(block);
            return false;
        }

        size_t pos = 0;
        while (pos + 8 <= block_size) {
            ext2_directory_t *entry = (ext2_directory_t *)(block + pos);

            if (entry->size < 8 || entry->size > block_size - pos) {
                break;
            }

            if (!entry->inode && entry->size >= need) {
                entry->inode = inode_num;
                entry->name_size = (u8)name_len;
                entry->type = type;
                memcpy(entry->name, name, name_len);

                bool ok = _write_block(priv, part, block_num, block);
                free(block);
                return ok;
            }

            size_t used = _dir_entry_size(entry->name_size);

            if (entry->inode != 0 && entry->size >= used + need) {
                ext2_directory_t *new_entry =
                    (ext2_directory_t *)((u8 *)entry + used);

                new_entry->inode = inode_num;
                new_entry->size = (u16)(entry->size - used);
                new_entry->name_size = (u8)name_len;
                new_entry->type = type;
                memcpy(new_entry->name, name, name_len);

                entry->size = (u16)used;

                bool ok = _write_block(priv, part, block_num, block);
                free(block);
                return ok;
            }

            pos += entry->size;
        }
    }

    u32 new_block = 0;
    bool changed = false;

    if (!_ensure_block(priv, part, dir_info, blocks, &new_block, &changed)) {
        free(block);
        return false;
    }

    memset(block, 0, block_size);

    ext2_directory_t *entry = (ext2_directory_t *)block;
    entry->inode = inode_num;
    entry->size = (u16)block_size;
    entry->name_size = (u8)name_len;
    entry->type = type;
    memcpy(entry->name, name, name_len);

    bool ok = _write_block(priv, part, new_block, block);
    free(block);

    if (!ok) {
        return false;
    }

    u64 new_size = (u64)(blocks + 1) * block_size;

    if (new_size > ext2_file_size(&dir_info->inode)) {
        _set_file_size(&dir_info->inode, new_size);
    }

    return true;
}

static bool _dir_remove_entry(
    ext2_private_t *priv,
    disk_partition_t *part,
    const ext2_node_info_t *dir_info,
    const char *name,
    u32 *inode_out,
    u8 *type_out
) {
    if (!priv || !part || !dir_info || !name || !name[0]) {
        return false;
    }

    u32 block_num = 0;
    size_t pos = 0;

    bool found_entry = _dir_find_entry(
        priv,
        part,
        dir_info,
        name,
        inode_out,
        &block_num,
        &pos,
        NULL,
        type_out
    );

    if (!found_entry) {
        return false;
    }

    u8 *block = malloc(priv->block_size);
    if (!block) {
        return false;
    }

    if (!_read_block(priv, part, block_num, block)) {
        free(block);
        return false;
    }

    ext2_directory_t *entry = (ext2_directory_t *)(block + pos);
    entry->inode = 0;
    entry->type = EXT2_DIR_UNKNOWN;

    bool ok = _write_block(priv, part, block_num, block);

    free(block);

    return ok;
}

static bool _dir_is_empty(
    ext2_private_t *priv,
    disk_partition_t *part,
    const ext2_inode_t *inode
) {
    if (!priv || !part || !inode) {
        return false;
    }

    u64 size = ext2_file_size(inode);
    if (!size) {
        return true;
    }

    u32 block_size = priv->block_size;
    u32 blocks = DIV_ROUND_UP(size, block_size);

    u8 *block = malloc(block_size);
    if (!block) {
        return false;
    }

    bool empty = true;

    for (u32 i = 0; i < blocks; i++) {
        u32 block_num = _block_for_index(priv, part, inode, i);
        if (!block_num) {
            continue;
        }

        if (!_read_block(priv, part, block_num, block)) {
            empty = false;
            break;
        }

        size_t pos = 0;
        while (pos + 8 <= block_size) {
            ext2_directory_t *entry = (ext2_directory_t *)(block + pos);

            if (entry->size < 8 || entry->size > block_size - pos) {
                empty = false;
                break;
            }

            if (entry->inode && entry->name_size) {
                if (
                    !(entry->name_size == 1 && entry->name[0] == '.') &&
                    !(entry->name_size == 2 && entry->name[0] == '.' && 
                    entry->name[1] == '.')
                ) {
                    empty = false;
                    break;
                }
            }

            pos += entry->size;
        }

        if (!empty) {
            break;
        }
    }

    free(block);
    return empty;
}

static bool _init_vnode(
    vfs_node_t *node,
    fs_instance_t *instance,
    u32 inode_num,
    const ext2_inode_t *inode
) {
    if (!node || !instance || !inode) {
        return false;
    }

    ext2_node_info_t *info = calloc(1, sizeof(ext2_node_info_t));
    if (!info) {
        return false;
    }

    info->inode_num = inode_num;
    info->inode = *inode;

    node->private = info;
    node->fs = instance;
    node->inode = inode_num;
    node->uid = inode->uid;
    node->gid = inode->gid;
    node->mode = inode->type & EXT2_IP_MASK;

    _sync_vnode(node, inode);

    return true;
}

static ssize_t
_read_file(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!node || !buf || !node->private || !node->fs) {
        return -1;
    }

    ext2_node_info_t *info = node->private;
    ext2_private_t *priv = node->fs->private;
    disk_partition_t *part = node->fs->partition;

    if (!priv || !part) {
        return -1;
    }

    u64 size = ext2_file_size(&info->inode);
    if (offset >= size) {
        return 0;
    }

    size_t to_read = len;
    if (offset + to_read > size) {
        to_read = (size_t)(size - offset);
    }

    u8 *out = buf;
    u32 block_size = priv->block_size;
    u8 *bounce = malloc(block_size);

    if (!bounce) {
        return -1;
    }

    size_t remaining = to_read;
    u64 cursor = offset;

    while (remaining) {
        u32 block_index = (u32)(cursor / block_size);
        size_t block_off = (size_t)(cursor % block_size);
        u32 block = _block_for_index(priv, part, &info->inode, block_index);

        if (!_read_block(priv, part, block, bounce)) {
            free(bounce);
            return -1;
        }

        size_t available = block_size - block_off;
        size_t chunk = remaining < available ? remaining : available;

        memcpy(out, bounce + block_off, chunk);

        out += chunk;
        remaining -= chunk;
        cursor += chunk;
    }

    free(bounce);

    u32 now = _now(priv);
    if (_should_update_atime(&info->inode, now)) {
        info->inode.last_access_time = now;

        if (_write_inode(priv, part, info->inode_num, &info->inode)) {
            _sync_vnode(node, &info->inode);
        }
    }

    return (ssize_t)(to_read - remaining);
}

static ssize_t
_write_file(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!node || !buf || !node->private || !node->fs) {
        return -1;
    }

    ext2_node_info_t *info = node->private;
    ext2_private_t *priv = node->fs->private;
    disk_partition_t *part = node->fs->partition;

    if (!priv || !part) {
        return -1;
    }

    if (!ext2_is_type(&info->inode, EXT2_IT_FILE)) {
        return -1;
    }

    if (!len) {
        return 0;
    }

    u32 block_size = priv->block_size;
    u8 *bounce = malloc(block_size);

    if (!bounce) {
        return -1;
    }

    u8 *in = buf;
    size_t remaining = len;
    u64 cursor = offset;
    bool inode_changed = false;

    while (remaining) {
        u32 block_index = (u32)(cursor / block_size);
        size_t block_off = (size_t)(cursor % block_size);

        u32 block = 0;
        bool changed = false;

        if (!_ensure_block(priv, part, info, block_index, &block, &changed)) {
            free(bounce);
            return -1;
        }

        inode_changed |= changed;

        size_t available = block_size - block_off;
        size_t chunk = remaining < available ? remaining : available;

        if (block_off || chunk < block_size) {
            if (!_read_block(priv, part, block, bounce)) {
                free(bounce);
                return -1;
            }
        } else {
            memset(bounce, 0, block_size);
        }

        memcpy(bounce + block_off, in, chunk);

        if (!_write_block(priv, part, block, bounce)) {
            free(bounce);
            return -1;
        }

        in += chunk;
        remaining -= chunk;
        cursor += chunk;
    }

    free(bounce);

    if (cursor > ext2_file_size(&info->inode)) {
        _set_file_size(&info->inode, cursor);
        inode_changed = true;
    }

    u32 now = _now(priv);

    info->inode.last_access_time = now;
    info->inode.last_modification_time = now;
    inode_changed = true;

    if (inode_changed && !_write_inode(priv, part, info->inode_num, &info->inode)) {
        return -1;
    }

    if (!_update_super_write_time(priv, part, now)) {
        return -1;
    }

    _sync_vnode(node, &info->inode);

    return (ssize_t)len;
}

static ssize_t _truncate_file(vfs_node_t *node, size_t len) {
    if (!node || !node->private || !node->fs) {
        return -1;
    }

    if (len != 0) {
        return -1;
    }

    ext2_node_info_t *info = node->private;
    ext2_private_t *priv = node->fs->private;
    disk_partition_t *part = node->fs->partition;

    if (!priv || !part) {
        return -1;
    }

    if (!_release_inode_blocks(priv, part, &info->inode)) {
        return -1;
    }

    u32 now = _now(priv);

    info->inode.last_access_time = now;
    info->inode.last_modification_time = now;

    if (!_write_inode(priv, part, info->inode_num, &info->inode)) {
        return -1;
    }

    if (!_update_super_write_time(priv, part, now)) {
        return -1;
    }

    _sync_vnode(node, &info->inode);

    return 0;
}

static ssize_t _dir_remove(vfs_node_t *node, char *name) {
    if (!node || !name || !name[0] || !node->private || !node->fs) {
        return -1;
    }

    ext2_node_info_t *parent_info = node->private;
    ext2_private_t *priv = node->fs->private;
    disk_partition_t *part = node->fs->partition;

    if (!priv || !part) {
        return -1;
    }

    if (!ext2_is_type(&parent_info->inode, EXT2_IT_DIR)) {
        return -1;
    }

    u32 inode_num = 0;
    u8 type = EXT2_DIR_UNKNOWN;

    bool found_entry = _dir_find_entry(
        priv,
        part,
        parent_info,
        name,
        &inode_num,
        NULL,
        NULL,
        NULL,
        &type
    );

    if (!found_entry) {
        return -1;
    }

    ext2_inode_t inode;
    if (!_read_inode(priv, part, inode_num, &inode)) {
        return -1;
    }

    bool is_dir = ext2_is_type(&inode, EXT2_IT_DIR);
    if (is_dir && !_dir_is_empty(priv, part, &inode)) {
        return -1;
    }

    if (!_dir_remove_entry(priv, part, parent_info, name, NULL, NULL)) {
        return -1;
    }

    if (!_release_inode_blocks(priv, part, &inode)) {
        return -1;
    }

    if (!_clear_inode(priv, part, inode_num)) {
        return -1;
    }

    if (!_free_inode(priv, part, inode_num)) {
        return -1;
    }

    if (is_dir && parent_info->inode.hard_link_count > 1) {
        parent_info->inode.hard_link_count--;
    }

    u32 now = _now(priv);

    parent_info->inode.last_access_time = now;
    parent_info->inode.last_modification_time = now;

    bool wrote_parent_inode = _write_inode(
        priv,
        part,
        parent_info->inode_num,
        &parent_info->inode
    );

    if (!wrote_parent_inode) {
        return -1;
    }

    if (!_update_super_write_time(priv, part, now)) {
        return -1;
    }

    _sync_vnode(node, &parent_info->inode);
    return 0;
}

static ssize_t _dir_create(vfs_node_t *node, vfs_node_t *child) {
    if (!node || !child || !node->private || !node->fs || !child->name) {
        return -1;
    }

    ext2_node_info_t *parent_info = node->private;
    ext2_private_t *priv = node->fs->private;
    disk_partition_t *part = node->fs->partition;

    if (!priv || !part) {
        return -1;
    }

    if (!ext2_is_type(&parent_info->inode, EXT2_IT_DIR)) {
        log_warn("create '%s' failed: parent is not a directory", child->name);
        return -1;
    }

    u16 inode_type = _vfs_to_inode_type(child->type);
    if (!inode_type) {
        log_warn(
            "create '%s' failed: unsupported vnode type %u",
            child->name,
            child->type
        );
        return -1;
    }

    u32 inode_num = 0;
    if (!_alloc_inode(priv, part, &inode_num)) {
        log_warn("create '%s' failed: no free inode", child->name);
        return -1;
    }

    ext2_inode_t inode = {0};
    u32 now = _now(priv);

    inode.type = inode_type | (child->mode & EXT2_IP_MASK);
    inode.uid = (u16)child->uid;
    inode.gid = (u16)child->gid;
    inode.last_access_time = now;
    inode.last_modification_time = now;
    inode.creation_time = now;
    inode.hard_link_count = (child->type == VFS_DIR) ? 2 : 1;

    if (child->type == VFS_DIR) {
        u32 block = 0;

        if (!_alloc_block(priv, part, &block)) {
            log_warn("create '%s' failed: no free data block", child->name);
            _free_inode(priv, part, inode_num);
            return -1;
        }

        inode.direct_block_ptr[0] = block;
        inode.disk_sector_count = priv->block_size / 512;
        _set_file_size(&inode, priv->block_size);

        bool wrote_dots = _dir_write_dots(
            priv,
            part,
            block,
            inode_num,
            parent_info->inode_num
        );

        if (!wrote_dots) {
            log_warn("create '%s' failed: dot entries write", child->name);
            _free_block(priv, part, block);
            _free_inode(priv, part, inode_num);
            return -1;
        }
    }

    if (!_write_inode(priv, part, inode_num, &inode)) {
        log_warn("create '%s' failed: inode write", child->name);
        _release_inode_blocks(priv, part, &inode);
        _free_inode(priv, part, inode_num);
        return -1;
    }

    u8 dir_type = _vfs_to_dir_type(child->type);

    bool added_parent_entry = _dir_add_entry(
        priv,
        part,
        parent_info,
        child->name,
        inode_num,
        dir_type
    );

    if (!added_parent_entry) {
        log_warn("create '%s' failed: parent entry insert", child->name);
        _release_inode_blocks(priv, part, &inode);
        _clear_inode(priv, part, inode_num);
        _free_inode(priv, part, inode_num);
        return -1;
    }

    if (child->type == VFS_DIR) {
        parent_info->inode.hard_link_count++;
    }

    parent_info->inode.last_access_time = now;
    parent_info->inode.last_modification_time = now;

    bool wrote_parent_inode = _write_inode(
        priv,
        part,
        parent_info->inode_num,
        &parent_info->inode
    );

    if (!wrote_parent_inode) {
        log_warn("create '%s' failed: parent inode write", child->name);
        return -1;
    }

    if (!_update_super_write_time(priv, part, now)) {
        log_warn(
            "create '%s' failed: superblock write time update", child->name
        );
        return -1;
    }

    if (!_init_vnode(child, node->fs, inode_num, &inode)) {
        log_warn("create '%s' failed: vnode init", child->name);
        return -1;
    }

    vfs_interface_t *iface = NULL;

    if (child->type == VFS_FILE) {
        iface = vfs_create_interface(_read_file, _write_file, _truncate_file);
    } else if (child->type == VFS_DIR) {
        iface = calloc(1, sizeof(vfs_interface_t));
        if (iface) {
            iface->create = _dir_create;
            iface->remove = _dir_remove;
        }
    }

    if ((child->type == VFS_FILE || child->type == VFS_DIR) && !iface) {
        log_warn("create '%s' failed: interface alloc", child->name);
        return -1;
    }

    child->interface = iface;

    _sync_vnode(node, &parent_info->inode);

    return 0;
}


static vfs_interface_t *_make_file_interface(void) {
    return vfs_create_interface(_read_file, _write_file, _truncate_file);
}

static vfs_interface_t *_make_dir_interface(void) {
    vfs_interface_t *iface = calloc(1, sizeof(vfs_interface_t));
    if (!iface) {
        return NULL;
    }

    iface->create = _dir_create;
    iface->remove = _dir_remove;

    return iface;
}

static bool _assign_interface(vfs_node_t *node, u32 vfs_type) {
    if (!node) {
        return false;
    }

    vfs_interface_t *iface = NULL;

    if (vfs_type == VFS_FILE) {
        iface = _make_file_interface();
    } else if (vfs_type == VFS_DIR) {
        iface = _make_dir_interface();
    }

    if ((vfs_type == VFS_FILE || vfs_type == VFS_DIR) && !iface) {
        return false;
    }

    node->interface = iface;

    return true;
}

static bool _build_dir(
    fs_instance_t *instance,
    vfs_node_t *parent,
    const ext2_inode_t *inode
) {
    if (!instance || !parent || !inode) {
        return false;
    }

    ext2_private_t *priv = instance->private;
    if (!priv) {
        return false;
    }

    u64 dir_size = ext2_file_size(inode);
    if (!dir_size) {
        return true;
    }

    u32 block_size = priv->block_size;
    u32 blocks = DIV_ROUND_UP(dir_size, block_size);
    u8 *block = malloc(block_size);

    if (!block) {
        return false;
    }

    for (u32 i = 0; i < blocks; i++) {
        u32 block_num = _block_for_index(priv, instance->partition, inode, i);
        if (!block_num) {
            continue;
        }

        if (!_read_block(priv, instance->partition, block_num, block)) {
            free(block);
            return false;
        }

        size_t pos = 0;
        size_t dir_off = (size_t)i * block_size;

        while (pos < block_size && dir_off + pos < dir_size) {
            if (block_size - pos < sizeof(ext2_directory_t)) {
                break;
            }

            ext2_directory_t *entry = (ext2_directory_t *)(block + pos);

            size_t entry_size = entry->size;
            if (entry_size < 8) {
                break;
            }

            if (entry_size > block_size - pos) {
                break;
            }

            u64 entry_end = (u64)dir_off + (u64)pos + entry_size;
            if (entry_end > dir_size) {
                break;
            }

            if (entry->inode && entry->name_size) {
                size_t name_len = entry->name_size;
                if (name_len > entry_size - 8) {
                    pos += entry_size;
                    continue;
                }

                if (name_len >= 256) {
                    name_len = 255;
                }

                char name[256];
                memcpy(name, entry->name, name_len);
                name[name_len] = '\0';

                if (strcmp(name, ".") && strcmp(name, "..")) {
                    ext2_inode_t child_inode;

                    bool read_child_inode = _read_inode(
                        priv,
                        instance->partition,
                        entry->inode,
                        &child_inode
                    );

                    if (read_child_inode) {
                        u32 vfs_type = _inode_type_to_vfs(&child_inode);
                        vfs_node_t *child = vfs_create(
                            parent,
                            name,
                            vfs_type,
                            child_inode.type & EXT2_IP_MASK
                        );

                        if (child) {
                            bool inited_child_vnode = _init_vnode(
                                child,
                                instance,
                                entry->inode,
                                &child_inode
                            );

                            if (!inited_child_vnode) {
                                log_warn("failed to init node %s", name);
                            }

                            if (vfs_type == VFS_FILE) {
                                if (!_assign_interface(child, VFS_FILE)) {
                                    log_warn(
                                        "failed to allocate interface for %s",
                                        name
                                    );
                                }
                            }

                            if (vfs_type == VFS_DIR) {
                                bool built_child_dir = _build_dir(
                                    instance,
                                    child,
                                    &child_inode
                                );
                                if (!built_child_dir) {
                                    log_warn("failed to build dir %s", name);
                                }

                                if (!_assign_interface(child, VFS_DIR)) {
                                    log_warn(
                                        "failed to allocate interface for %s",
                                        name
                                    );
                                }
                            }
                        }
                    }
                }
            }

            pos += entry_size;
        }
    }

    free(block);
    return true;
}

static fs_instance_t *_probe(disk_partition_t *part) {
    if (!part || !part->disk || !part->disk->interface || !part->disk->interface->read) {
        return NULL;
    }

    ext2_private_t *priv = calloc(1, sizeof(ext2_private_t));
    if (!priv) {
        return NULL;
    }

    if (!_ext2_read(part, &priv->superblock, 1024, sizeof(ext2_superblock_t))) {
        free(priv);
        return NULL;
    }

    if (priv->superblock.signature != EXT2_SIGNATURE) {
        free(priv);
        return NULL;
    }

    u32 hz = arch_timer_hz();
    u64 ticks = arch_timer_ticks();

    if (priv->superblock.last_write_time) {
        priv->time_base = priv->superblock.last_write_time;
    } else if (hz) {
        priv->time_base = (u32)(ticks / hz);
    } else {
        priv->time_base = 0;
    }

    priv->time_tick_base = ticks;

    priv->block_size = ext2_block_size(&priv->superblock);
    priv->inode_size = ext2_inode_size(&priv->superblock);
    priv->group_count = ext2_group_count(&priv->superblock);

    priv->gdt_size =
        (size_t)priv->group_count * sizeof(ext2_group_descriptor_t);

    priv->groups = malloc(priv->gdt_size);

    if (!priv->groups) {
        free(priv);
        return NULL;
    }

    size_t cache_bytes = (size_t)EXT2_BLOCK_CACHE_SIZE * priv->block_size;
    if (priv->block_size && cache_bytes / priv->block_size == EXT2_BLOCK_CACHE_SIZE) {
        priv->cache_data = malloc(cache_bytes);
    }

    if (priv->cache_data) {
        memset(priv->cache_data, 0, cache_bytes);
        for (size_t i = 0; i < EXT2_BLOCK_CACHE_SIZE; i++) {
            priv->cache[i].data = priv->cache_data + i * priv->block_size;
        }
    }

    priv->gdt_offset =
        (size_t)(priv->superblock.superblock_offset + 1) * priv->block_size;

    if (!_ext2_read(part, priv->groups, priv->gdt_offset, priv->gdt_size)) {
        free(priv->cache_data);
        free(priv->groups);
        free(priv);
        return NULL;
    }

    fs_instance_t *instance = calloc(1, sizeof(fs_instance_t));
    if (!instance) {
        free(priv->cache_data);
        free(priv->groups);
        free(priv);
        return NULL;
    }

    instance->private = priv;
    instance->has_tree = false;
    instance->subtree_root = NULL;

    if (priv->superblock.fs_state != EXT2_FS_CLEAN) {
        log_warn("filesystem marked dirty");
    }

    log_debug("filesystem detected");
    return instance;
}

static bool _build_tree(fs_instance_t *instance) {
    if (!instance || !instance->private || !instance->partition) {
        return false;
    }

    ext2_private_t *priv = instance->private;
    ext2_inode_t root_inode;

    if (!_read_inode(priv, instance->partition, EXT2_ROOT_INODE, &root_inode)) {
        return false;
    }

    vfs_node_t *root = vfs_create_node(NULL, VFS_DIR);
    if (!root) {
        return false;
    }

    if (!_init_vnode(root, instance, EXT2_ROOT_INODE, &root_inode)) {
        vfs_destroy_node(root);
        return false;
    }

    instance->subtree_root = root->tree_entry;
    instance->has_tree = true;

    if (!_build_dir(instance, root, &root_inode)) {
        log_warn("failed to build directory tree");
    }

    if (!_assign_interface(root, VFS_DIR)) {
        vfs_destroy_node(root);
        return false;
    }

    return true;
}

static bool
_node_chmod(fs_instance_t *instance, vfs_node_t *node, mode_t mode) {
    if (!instance || !instance->private || !instance->partition || !node || !node->private) {
        return false;
    }

    ext2_private_t *priv = instance->private;
    ext2_node_info_t *info = node->private;
    u32 now = _now(priv);

    info->inode.type =
        (info->inode.type & EXT2_IT_MASK) | (mode & EXT2_IP_MASK);

    bool wrote_inode = _write_inode(
        priv,
        instance->partition,
        info->inode_num,
        &info->inode
    );

    if (!wrote_inode) {
        return false;
    }

    if (!_update_super_write_time(priv, instance->partition, now)) {
        return false;
    }

    _sync_vnode(node, &info->inode);
    return true;
}

static bool
_node_chown(fs_instance_t *instance, vfs_node_t *node, uid_t uid, gid_t gid) {
    if (!instance || !instance->private || !instance->partition || !node || !node->private) {
        return false;
    }

    ext2_private_t *priv = instance->private;
    ext2_node_info_t *info = node->private;
    u32 now = _now(priv);

    info->inode.uid = (u16)uid;
    info->inode.gid = (u16)gid;

    bool wrote_inode = _write_inode(
        priv,
        instance->partition,
        info->inode_num,
        &info->inode
    );

    if (!wrote_inode) {
        return false;
    }

    if (!_update_super_write_time(priv, instance->partition, now)) {
        return false;
    }

    _sync_vnode(node, &info->inode);
    return true;
}

static bool _free_vnode(const void *data, void *private) {
    (void)private;

    vfs_node_t *vnode = (vfs_node_t *)data;
    if (!vnode) {
        return false;
    }

    if (vnode->private) {
        free(vnode->private);
    }

    if (vnode->interface) {
        free(vnode->interface);
    }

    if (vnode->name) {
        free(vnode->name);
    }

    free(vnode);
    return false;
}

static bool _destroy_tree(fs_instance_t *instance) {
    if (!instance || !instance->subtree_root) {
        return false;
    }

    tree_node_t *root = instance->subtree_root;

    tree_foreach_node(root, _free_vnode, NULL);
    tree_prune(root);

    instance->subtree_root = NULL;
    instance->has_tree = false;

    return true;
}

bool ext2fs_init(void) {
    static fs_interface_t ext2_interface = {
        .probe = _probe,
        .build_tree = _build_tree,
        .destroy_tree = _destroy_tree,
    };

    static fs_interface_t ext2_node_interface = {
        .chmod = _node_chmod,
        .chown = _node_chown,
    };

    static fs_t ext2_fs = {
        .name = "ext2",
        .fs_interface = &ext2_interface,
        .node_interface = &ext2_node_interface,
        .private = NULL,
    };

    bool ok = file_system_register(&ext2_fs);

    if (ok) {
        log_debug("registered");
    }

    return ok;
}
