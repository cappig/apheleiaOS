#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define MBR_SIGNATURE 0xaa55

// Common MBR partition types
// https://en.wikipedia.org/wiki/Partition_type
enum mbr_partition_type {
    MBR_EMPTY = 0x00,
    MBR_FAT12 = 0x01,
    MBR_FAT16_SMALL = 0x04,
    MBR_EBR1 = 0x05,
    MBR_FAT16_LARGE = 0x06,
    MBR_NTFS = 0x07,
    MBR_FAT32_CHS = 0x0b,
    MBR_FAT32_LBA = 0x0c,
    MBR_FAT16_LBA = 0x0e,
    MBR_EBR2 = 0x0f,
    MBR_LINUX_SWAP = 0x82,
    MBR_LINUX = 0x83,
    MBR_EBR3 = 0x85,
    MBR_ISO = 0x96,
    MBR_GPT = 0xee,
    MBR_UNKNOWN = 0xff,
};

enum mbr_status {
    MBR_INACTIVE = 0x00,
    MBR_BOOTABLE = 0x80,
};

typedef struct PACKED {
    u8 status;
    u8 chs_first[3];
    u8 type;
    u8 chs_last[3];
    u32 lba_first;
    u32 sector_count;
} mbr_partition_t;

typedef struct {
    mbr_partition_t partitions[4];
} mbr_table_t;

typedef struct PACKED {
    u8 bootstrap[446];
    mbr_table_t table;
    u16 signature;
} mbr_t;
