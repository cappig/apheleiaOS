#include "ext2fs.h"

#include <base/macros.h>
#include <fs/ext2.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>

#include "sys/disk.h"
#include "sys/vfs.h"

typedef struct {
    ext2_superblock_t superblock;
    ext2_group_descriptor_t* groups;
    u32 group_count;
    u32 block_size;
    u32 inode_size;
} ext2_private_t;

typedef struct {
    u32 inode_num;
    ext2_inode_t inode;
} ext2_node_info_t;

static bool _ext2_read(disk_partition_t* part, void* dest, size_t offset, size_t bytes) {
    if (!part || !part->disk || !part->disk->interface || !part->disk->interface->read)
        return false;

    ssize_t read = part->disk->interface->read(part->disk, dest, part->offset + offset, bytes);
    return read == (ssize_t)bytes;
}

static bool _ext2_read_block(ext2_private_t* priv, disk_partition_t* part, u32 block, void* dest) {
    if (!priv || !part || !dest)
        return false;

    if (!block) {
        memset(dest, 0, priv->block_size);
        return true;
    }

    size_t offset = (size_t)block * priv->block_size;
    return _ext2_read(part, dest, offset, priv->block_size);
}

static bool
_ext2_read_inode(ext2_private_t* priv, disk_partition_t* part, u32 inode_num, ext2_inode_t* inode) {
    if (!priv || !part || !inode || inode_num == 0)
        return false;

    u32 group = (inode_num - 1) / priv->superblock.inodes_in_group;
    u32 index = (inode_num - 1) % priv->superblock.inodes_in_group;

    if (group >= priv->group_count)
        return false;

    ext2_group_descriptor_t* gd = &priv->groups[group];

    size_t inode_table = (size_t)gd->inode_table_offset * priv->block_size;
    size_t inode_offset = inode_table + (size_t)index * priv->inode_size;

    memset(inode, 0, sizeof(ext2_inode_t));

    size_t read_size = priv->inode_size;
    if (read_size > sizeof(ext2_inode_t))
        read_size = sizeof(ext2_inode_t);

    return _ext2_read(part, inode, inode_offset, read_size);
}

static u32 _read_indirect(ext2_private_t* priv, disk_partition_t* part, u32 block, u32 index) {
    if (!block)
        return 0;

    u32* table = malloc(priv->block_size);
    if (!table)
        return 0;

    if (!_ext2_read_block(priv, part, block, table)) {
        free(table);
        return 0;
    }

    u32 result = table[index];
    free(table);
    return result;
}

static u32 _ext2_block_for_index(
    ext2_private_t* priv,
    disk_partition_t* part,
    const ext2_inode_t* inode,
    u32 block_index
) {
    if (!priv || !inode)
        return 0;

    u32 n = priv->block_size / sizeof(u32);

    if (block_index < 12)
        return inode->direct_block_ptr[block_index];

    block_index -= 12;

    // Single indirect
    if (block_index < n)
        return _read_indirect(priv, part, inode->indirect_block_ptr[0], block_index);

    block_index -= n;

    // Double indirect
    if (block_index < n * n) {
        u32 outer = _read_indirect(priv, part, inode->indirect_block_ptr[1], block_index / n);
        return _read_indirect(priv, part, outer, block_index % n);
    }

    block_index -= n * n;

    // Triple indirect
    if (block_index < n * n * n) {
        u32 l1 = _read_indirect(priv, part, inode->indirect_block_ptr[2], block_index / (n * n));
        u32 l2 = _read_indirect(priv, part, l1, (block_index / n) % n);
        return _read_indirect(priv, part, l2, block_index % n);
    }

    return 0;
}

static u32 _ext2_inode_type_to_vfs(const ext2_inode_t* inode) {
    if (!inode)
        return VFS_FILE;

    if (ext2_is_type(inode, EXT2_IT_DIR))
        return VFS_DIR;

    if (ext2_is_type(inode, EXT2_IT_FILE))
        return VFS_FILE;

    if (ext2_is_type(inode, EXT2_IT_CHAR_DEV))
        return VFS_CHARDEV;

    if (ext2_is_type(inode, EXT2_IT_BLOCK_DEV))
        return VFS_BLOCKDEV;

    return VFS_FILE;
}

static bool
_ext2_init_vnode(vfs_node_t* node, fs_instance_t* instance, u32 inode_num, const ext2_inode_t* inode) {
    if (!node || !instance || !inode)
        return false;

    ext2_node_info_t* info = calloc(1, sizeof(ext2_node_info_t));
    if (!info)
        return false;

    info->inode_num = inode_num;
    info->inode = *inode;

    node->private = info;
    node->fs = instance;
    node->inode = inode_num;
    node->uid = inode->uid;
    node->gid = inode->gid;
    node->mode = inode->type & EXT2_IP_MASK;
    node->size = ext2_file_size(inode);
    node->time.accessed = inode->last_access_time;
    node->time.created = inode->creation_time;
    node->time.modified = inode->last_modification_time;

    return true;
}

static ssize_t _ext2_read_file(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)flags;

    if (!node || !buf || !node->private || !node->fs)
        return -1;

    ext2_node_info_t* info = node->private;
    ext2_private_t* priv = node->fs->private;

    if (!priv || !node->fs->partition)
        return -1;

    u64 size = ext2_file_size(&info->inode);
    if (offset >= size)
        return 0;

    size_t to_read = len;
    if (offset + to_read > size)
        to_read = (size_t)(size - offset);

    u8* out = buf;
    u32 block_size = priv->block_size;
    u8* bounce = malloc(block_size);

    if (!bounce)
        return -1;

    size_t remaining = to_read;
    u64 cursor = offset;

    while (remaining) {
        u32 block_index = (u32)(cursor / block_size);
        size_t block_off = (size_t)(cursor % block_size);
        u32 block = _ext2_block_for_index(priv, node->fs->partition, &info->inode, block_index);

        if (!_ext2_read_block(priv, node->fs->partition, block, bounce)) {
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
    return (ssize_t)(to_read - remaining);
}

static bool _ext2_build_dir(fs_instance_t* instance, vfs_node_t* parent, const ext2_inode_t* inode) {
    if (!instance || !parent || !inode)
        return false;

    ext2_private_t* priv = instance->private;
    if (!priv)
        return false;

    u64 dir_size = ext2_file_size(inode);
    if (!dir_size)
        return true;

    u32 block_size = priv->block_size;
    u32 blocks = DIV_ROUND_UP(dir_size, block_size);
    u8* block = malloc(block_size);

    if (!block)
        return false;

    for (u32 i = 0; i < blocks; i++) {
        u32 block_num = _ext2_block_for_index(priv, instance->partition, inode, i);

        if (!_ext2_read_block(priv, instance->partition, block_num, block)) {
            free(block);
            return false;
        }

        size_t pos = 0;
        size_t dir_off = (size_t)i * block_size;

        while (pos < block_size && dir_off + pos < dir_size) {
            if (block_size - pos < sizeof(ext2_directory_t))
                break;

            ext2_directory_t* entry = (ext2_directory_t*)(block + pos);

            size_t entry_size = entry->size;
            if (entry_size < 8)
                break;

            if (entry_size > block_size - pos)
                break;

            u64 entry_end = (u64)dir_off + (u64)pos + entry_size;
            if (entry_end > dir_size)
                break;

            if (entry->inode && entry->name_size) {
                size_t name_len = entry->name_size;
                if (name_len > entry_size - 8) {
                    pos += entry_size;
                    continue;
                }
                if (name_len >= 256)
                    name_len = 255;

                char name[256];
                memcpy(name, entry->name, name_len);
                name[name_len] = '\0';

                if (strcmp(name, ".") && strcmp(name, "..")) {
                    ext2_inode_t child_inode;

                    if (_ext2_read_inode(priv, instance->partition, entry->inode, &child_inode)) {
                        u32 vfs_type = _ext2_inode_type_to_vfs(&child_inode);
                        vfs_node_t* child =
                            vfs_create(parent, name, vfs_type, child_inode.type & EXT2_IP_MASK);

                        if (child) {
                            if (!_ext2_init_vnode(child, instance, entry->inode, &child_inode))
                                log_warn("ext2: failed to init node %s", name);

                            if (vfs_type == VFS_FILE) {
                                child->interface = vfs_create_interface(_ext2_read_file, NULL);
                                if (!child->interface)
                                    log_warn("ext2: failed to allocate interface for %s", name);
                            }

                            if (vfs_type == VFS_DIR)
                                _ext2_build_dir(instance, child, &child_inode);
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

static fs_instance_t* _ext2_probe(disk_partition_t* part) {
    if (!part || !part->disk || !part->disk->interface || !part->disk->interface->read)
        return NULL;

    ext2_private_t* priv = calloc(1, sizeof(ext2_private_t));
    if (!priv)
        return NULL;

    if (!_ext2_read(part, &priv->superblock, 1024, sizeof(ext2_superblock_t))) {
        free(priv);
        return NULL;
    }

    if (priv->superblock.signature != EXT2_SIGNATURE) {
        free(priv);
        return NULL;
    }

    priv->block_size = ext2_block_size(&priv->superblock);
    priv->inode_size = ext2_inode_size(&priv->superblock);
    priv->group_count = ext2_group_count(&priv->superblock);

    size_t gdt_size = (size_t)priv->group_count * sizeof(ext2_group_descriptor_t);
    priv->groups = malloc(gdt_size);

    if (!priv->groups) {
        free(priv);
        return NULL;
    }

    size_t gdt_offset = (size_t)(priv->superblock.superblock_offset + 1) * priv->block_size;

    if (!_ext2_read(part, priv->groups, gdt_offset, gdt_size)) {
        free(priv->groups);
        free(priv);
        return NULL;
    }

    fs_instance_t* instance = calloc(1, sizeof(fs_instance_t));
    if (!instance) {
        free(priv->groups);
        free(priv);
        return NULL;
    }

    instance->private = priv;
    instance->has_tree = false;
    instance->subtree_root = NULL;

    if (priv->superblock.fs_state != EXT2_FS_CLEAN)
        log_warn("ext2: filesystem marked dirty");

    log_debug("ext2: filesystem detected");
    return instance;
}

static bool _ext2_build_tree(fs_instance_t* instance) {
    if (!instance || !instance->private || !instance->partition)
        return false;

    ext2_private_t* priv = instance->private;
    ext2_inode_t root_inode;

    if (!_ext2_read_inode(priv, instance->partition, EXT2_ROOT_INODE, &root_inode))
        return false;

    vfs_node_t* root = vfs_create_node(NULL, VFS_DIR);
    if (!root)
        return false;

    if (!_ext2_init_vnode(root, instance, EXT2_ROOT_INODE, &root_inode)) {
        vfs_destroy_node(root);
        return false;
    }

    instance->subtree_root = root->tree_entry;
    instance->has_tree = true;

    if (!_ext2_build_dir(instance, root, &root_inode))
        log_warn("ext2: failed to build directory tree");

    return true;
}

static bool _ext2_free_vnode(const void* data, void* private) {
    (void)private;

    vfs_node_t* vnode = (vfs_node_t*)data;
    if (!vnode)
        return false;

    if (vnode->private)
        free(vnode->private);

    if (vnode->interface)
        free(vnode->interface);

    if (vnode->name)
        free(vnode->name);

    free(vnode);
    return false;
}

static bool _ext2_destroy_tree(fs_instance_t* instance) {
    if (!instance || !instance->subtree_root)
        return false;

    tree_node_t* root = instance->subtree_root;
    tree_foreach_node(root, _ext2_free_vnode, NULL);
    tree_prune(root);

    ext2_private_t* priv = instance->private;
    if (priv) {
        free(priv->groups);
        free(priv);
    }
    instance->private = NULL;

    instance->subtree_root = NULL;
    instance->has_tree = false;
    return true;
}

bool ext2fs_init(void) {
    static fs_interface_t ext2_interface = {
        .probe = _ext2_probe,
        .build_tree = _ext2_build_tree,
        .destroy_tree = _ext2_destroy_tree,
    };

    static fs_t ext2_fs = {
        .name = "ext2",
        .fs_interface = &ext2_interface,
        .node_interface = NULL,
        .private = NULL,
    };

    bool ok = file_system_register(&ext2_fs);
    if (ok)
        log_info("ext2: registered");
    return ok;
}
