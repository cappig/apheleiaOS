#pragma once

#include <base/attributes.h>
#include <base/types.h>

// EXT2 filesystem structures
// https://wiki.osdev.org/Ext2
// https://cscie28.dce.harvard.edu/lectures/lect04/6_Extras/ext2-struct.html
// https://www.nongnu.org/ext2-doc/ext2.html

#define EXT2_SIGNATURE 0xef53

typedef struct PACKED {
    u32 inode_count;
    u32 block_count;
    u32 reserved_block_count;

    u32 free_block_count;
    u32 free_inode_count;

    u32 superblock_offset;

    u32 block_size_shift;
    u32 fragment_size_shift;

    u32 blocks_in_group;
    u32 fragments_in_group;
    u32 inodes_in_group;

    u32 last_mount_time;
    u32 last_write_time;

    u16 mounts_since_fsck;
    u16 max_mounts_until_fsck;

    u16 signature;

    u16 fs_state;
    u16 error_behavior;
    u16 version_minor;

    u32 last_fsck_time;
    u32 fsck_interval_time;
    u32 fs_creator_os;
    u32 version_major;

    u16 uid_reserved;
    u16 gid_reserved;

    // Extended superblock, present if version_major >= 1
    u32 first_inode;
    u16 inode_size;
    u16 superblock_group;

    u32 optional_features;
    u32 required_features;
    u32 write_features;

    u8 fs_id[16];
    u8 volume_name[16];
    u8 last_mount_path[64];

    u32 compression_algo;

    u8 file_prealloc_blocks;
    u8 dir_prealloc_blocks;

    u16 _reserved0;

    u8 journal_id[16];
    u32 journal_inode;
    u32 journal_device;
    u32 orphan_list_head;

    // some other stuff might be here ...
    u8 _reserved1[788];
} ext2_superblock;

enum ext2_fs_state {
    EXT2_FS_CLEAN = 1,
    EXT2_FS_HAS_ERRORS = 2,
};

enum ext2_error_behavior {
    EXT2_ERROR_IGNORE = 1,
    EXT2_ERROR_REMOUNT = 2,
    EXT2_ERROR_PANIC = 3,
};

enum ext2_optional_features {
    EXT2_OF_DIR_PREALLOCATE = (1 << 0),
    EXT2_OF_HAS_AFS = (1 << 1),
    EXT2_OF_HAS_JOURNAL = (1 << 2),
    EXT2_OF_EXTENDED_INODES = (1 << 3),
    EXT2_OF_FS_RESIZE = (1 << 4),
    EXT2_OF_DIR_HASH_INDEX = (1 << 5),
};

enum ext2_required_features {
    EXT2_RF_COMPRESSION = (1 << 0),
    EXT2_RF_DIR_HAS_TYPE = (1 << 1),
    EXT2_RF_JOURNAL_REPLAY = (1 << 2),
    EXT2_RF_JOURNAL_DEVICE = (1 << 3),
};

enum ext2_write_features {
    EXT2_WF_SPARSE_SUPERBLOCK = (1 << 0),
    EXT2_WF_64BIT = (1 << 1),
    EXT2_WF_DIR_BTREE = (1 << 2),
};


typedef struct PACKED {
    u32 usage_bitmap_offset;
    u32 inode_bitmap_offset;

    u32 inode_table_offset;

    u32 unallocated_block_count;
    u32 unallocated_inode_count;

    u32 directory_count;

    u8 _padding0[14];
} ext2_group_descriptor;


typedef struct PACKED {
    u16 type;
    u16 uid;
    u32 size_low;

    u32 last_access_time;
    u32 creation_time;
    u32 last_modification_time;
    u32 deletion_time;

    u16 gid;

    u16 hard_link_count;
    u32 disk_sector_count;

    u32 flags;

    u32 os_specific0; // not used

    u32 direct_block_ptr[12];
    u32 indirect_block_ptr[3];

    u32 generation_num;

    u32 acl_block;
    u32 size_high;

    u32 fragment_offset;

    u8 os_specific1[12];
} ext2_inode;

#define EXT2_IT_MASK 0xf000

#define EXT2_IS_TYPE(field, type) (((field) & EXT2_IT_MASK) == type)

enum ext2_inode_type {
    EXT2_IT_FIFO = 0x1000,
    EXT2_IT_CHAR_DEV = 0x2000,
    EXT2_IT_DIR = 0x4000,
    EXT2_IT_BLOCK_DEV = 0x6000,
    EXT2_IT_FILE = 0x8000,
    EXT2_IT_SYMLINK = 0xa000,
    EXT2_IT_SOCKET = 0xc000,
};

#define EXT2_IP_MASK 0x0fff

enum ext2_inode_permission {
    EXT2_IP_OTHER_EXECUTE = 0x001,
    EXT2_IP_OTHER_WRITE = 0x002,
    EXT2_IP_OTHER_READ = 0x004,

    EXT2_IP_GROUP_EXECUTE = 0x008,
    EXT2_IP_GROUP_WRITE = 0x010,
    EXT2_IP_GROUP_READ = 0x020,

    EXT2_IP_USER_EXECUTE = 0x040,
    EXT2_IP_USER_WRITE = 0x080,
    EXT2_IP_USER_READ = 0x100,

    EXT2_IP_STICKY = 0x200,

    EXT2_IP_SET_GID = 0x400,
    EXT2_IP_SET_UID = 0x400,
};

enum ext2_inode_flags {
    EX2_IF_SECURE_DELETION = (1 << 0), // not used
    EX2_IF_KEEP_ON_DELETE = (1 << 1), // not used
    EX2_IF_COMPRESSION = (1 << 2), // not used
    EX2_IF_SYNCHRONOUS = (1 << 3),
    EX2_IF_IMMUTABLE = (1 << 4),
    EX2_IF_APPEND_ONLY = (1 << 5),
    EX2_IF_NO_DUMP = (1 << 6),
    EX2_IF_DONT_UPDATE_TIME = (1 << 7),
    // ... reserved range
    EX2_IF_HASH_INDEX = (1 << 16),
    EX2_IF_AFS = (1 << 17),
    EX2_IF_JOURNAL = (1 << 18),
};

typedef struct PACKED {
    u32 inode;
    u16 size;
    u8 name_size;
    u8 type;
    char name[];
} ext2_directory;

enum ext2_directory_type {
    EXT2_DIR_UNKNOWN = 0,
    EXT2_DIR_REGULAR = 1,
    EXT2_DIR_DIRECTORY = 2,
    EXT2_DIR_CHAR_DEV = 3,
    EXT2_DIR_BLOCK_DEV = 4,
    EXT2_DIR_FIFO = 5,
    EXT2_DIR_SOCKET = 6,
    EXT2_DIR_SYMLINK = 7,
};
