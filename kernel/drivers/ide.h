#pragma once

#include <base/addr.h>
#include <base/attributes.h>
#include <base/types.h>
#include <x86/asm.h>

#include "vfs/fs.h"

// https://wiki.osdev.org/ATAPI
// https://wiki.osdev.org/PCI_IDE_Controller
// https://wiki.osdev.org/ATA/ATAPI_using_DMA
// https://people.freebsd.org/~imp/asiabsdcon2015/works/d2161r5-ATAATAPI_Command_Set_-_3.pdf
// https://pdos.csail.mit.edu/6.828/2005/readings/hardware/ATA-d1410r3a.pdf

#define ATA_SECTOR_SIZE   512
#define ATAPI_SECTOR_SIZE 2048

#define ATA_PRIMARY_BASE      0x1f0
#define ATA_PRIMARY_CONTROL   0x3f6
#define ATA_SECONDARY_BASE    0x170
#define ATA_SECONDARY_CONTROL 0x376

#define ATA_DEVICE_CHS 0xa0
#define ATA_DEVICE_LBA 0xe0

#define TYPE_IS_ATAPI(type) ((type) == ATA_DEV_PATAPI || (type) == ATA_DEV_SATAPI)
#define TYPE_IS_ATA(type)   ((type) == ATA_DEV_PATA || (type) == ATA_DEV_SATA)

typedef enum {
    ATA_DEV_PATAPI = 0xeb14,
    ATA_DEV_SATAPI = 0x9669,
    ATA_DEV_PATA = 0x0000,
    ATA_DEV_SATA = 0xc33c,

    ATA_DEV_NONE = 0xffff,
} ata_device_type;

typedef enum {
    ATA_SR_BUSY = 0x80,
    ATA_SR_READY = 0x40,
    ATA_SR_DRIVE_FAULT = 0x20,
    ATA_SR_DRIVE_SEEK_DONE = 0x10,
    ATA_SR_DATA_REQUEST_READY = 0x08,
    ATA_SR_CORRECTED = 0x04,
    ATA_SR_INDEX = 0x02,
    ATA_SR_ERROR = 0x01,
} ata_status;

typedef enum {
    ATA_ER_BAD_BLOCK = 0x80,
    ATA_ER_UNCORRECTABLE_DATA = 0x40,
    ATA_ER_MEDIA_CHANGED = 0x20,
    ATA_ER_NO_ID = 0x10,
    ATA_ER_MEDIA_CHANGE_REQUEST = 0x08,
    ATA_ER_COMMAND_ABORTED = 0x04,
    ATA_ER_NO_TRACK_ZERO = 0x02,
    ATA_ER_NO_ADDRESS = 0x01,
} ata_error;

typedef enum {
    ATA_CMD_READ_PIO = 0x20,
    ATA_CMD_READ_PIO_EXT = 0x24,
    ATA_CMD_READ_DMA = 0xc8,
    ATA_CMD_READ_DMA_EXT = 0x25,
    ATA_CMD_WRITE_PIO = 0x30,
    ATA_CMD_WRITE_PIO_EXT = 0x34,
    ATA_CMD_WRITE_DMA = 0xca,
    ATA_CMD_WRITE_DMA_EXT = 0x35,
    ATA_CMD_CACHE_FLUSH = 0xe7,
    ATA_CMD_CACHE_FLUSH_EXT = 0xea,
    ATA_CMD_PACKET = 0xa0,
    ATA_CMD_IDENTIFY_PACKET = 0xa1,
    ATA_CMD_IDENTIFY = 0xec,
    ATA_CMD_DIAGNOSTIC = 0x90,

    // https://ulinktech.com/wp-content/uploads/2020/05/ATAPI-Command-Table-in-OpCode-Order.pdf
    ATAPI_CMD_READ = 0xa8,
    ATAPI_CMD_CAPACITY = 0x25,
    ATAPI_CMD_EJECT = 0x1b,
} ata_command;

typedef enum {
    ATA_CNT_NO_INT = 1 << 1,
    ATA_CNT_RESET = 1 << 2,
    ATA_CNT_HIGH_ORDER_BYTE = 1 << 7,
} ata_control_bits;

typedef enum {
    ATA_REG_DATA = 0x00,
    ATA_REG_ERROR = 0x01,
    ATA_REG_FEATURES = 0x01,
    ATA_REG_SECCOUNT0 = 0x02,
    ATA_REG_LBA0 = 0x03,
    ATA_REG_LBA1 = 0x04,
    ATA_REG_LBA2 = 0x05,
    ATA_REG_DEVICE = 0x06,
    ATA_REG_COMMAND = 0x07,
    ATA_REG_STATUS = 0x07,
    ATA_REG_SECCOUNT1 = 0x08,
    ATA_REG_LBA3 = 0x09,
    ATA_REG_LBA4 = 0x0a,
    ATA_REG_LBA5 = 0x0b,
    ATA_REG_CONTROL = 0x0c,
    ATA_REG_ALTSTATUS = 0x0c,
    ATA_REG_DEVADDRESS = 0x0d,
} ata_registers;

typedef enum {
    // When this bit is set the channel is in DMA mode
    ATA_BM_CMD_DMA = 1 << 1,
    // When this bit is set channel is in read mode
    // When unset channel is in write mode
    ATA_BM_CMD_READ = 1 << 3,
} ata_bm_commands;

// ATAPI can read SCSI packets
typedef union {
    u8 bytes[12];
    u16 words[6];
} atapi_packet;

// Defined in table 45 of the T13 ATA spec
// This struct contains a lot of irrelevant and deprecated fields so we
// just define the ones we care about
typedef union PACKED {
    struct {
        u16 info;
        u16 _unused0[26];
        char model[40];
        u16 _unused1[13];
        u32 short_lba_sectors;
        u16 _unused3[38];
        u64 long_lba_sectors;
        u16 _unused4[2];
        u16 physical_sector_size;
        u16 _unused5[10];
        u32 logical_sector_size;
        u16 _unused6[90];
        u16 alignment;
        u16 _unused7[45];
        u16 checksum;
    };
    u16 raw[256];
} ata_identify;

// TODO: figure out the addr mode
typedef enum {
    ATA_ADDR_CHS,
    ATA_ADDR_LBA28,
    ATA_ADDR_LBA48,
} ata_address_mode;

// Only applies to non packet devices
typedef struct PACKED {
    u16 base;
    u16 control;
    u16 bus_master;
    u16 no_int;
} ide_channel;

// 0th bit: is_slave, 1st bit: is_secondary
typedef enum {
    ATA_PRIMARY_MASTER = 0b00,
    ATA_PRIMARY_SLAVE = 0b01,
    ATA_SECONDARY_MASTER = 0b10,
    ATA_SECONDARY_SLAVE = 0b11,
} ide_disk_number;

typedef struct {
    // If false all other fields may be garbage
    bool exists;

    bool is_master;
    bool is_atapi;

    u32 sector_size;
    u64 sectors;

    u8* read_buffer;

    ide_channel* channel;

    ata_identify* identify;
} ide_device;

typedef struct {
    ide_channel primary;
    ide_channel secondary;

    // How many valid disks does this controller have [0, 4]
    usize disks;

    // Indexed by `ide_disk_number`
    ide_device devices[4];
} ide_controller;

typedef struct {
    u16* buffer;
    usize lba[4];
    bool valid[4];
} atapi_cache;


bool ide_disk_init(virtual_fs* vfs);
