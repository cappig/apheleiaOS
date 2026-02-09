#include "ata.h"

#include <base/types.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <x86/asm.h>

#include "sys/disk.h"

#define ATA_PRIMARY_BASE 0x1f0
#define ATA_PRIMARY_CTRL 0x3f6

#define ATA_SECTOR_SIZE 512

#define ATA_REG_DATA     0x00
#define ATA_REG_ERROR    0x01
#define ATA_REG_SECCOUNT 0x02
#define ATA_REG_LBA0     0x03
#define ATA_REG_LBA1     0x04
#define ATA_REG_LBA2     0x05
#define ATA_REG_DEVICE   0x06
#define ATA_REG_STATUS   0x07
#define ATA_REG_COMMAND  0x07

#define ATA_CMD_IDENTIFY 0xec
#define ATA_CMD_READ_PIO 0x20

#define ATA_SR_BUSY  0x80
#define ATA_SR_READY 0x40
#define ATA_SR_DF    0x20
#define ATA_SR_DRQ   0x08
#define ATA_SR_ERR   0x01

#define ATA_CTRL_NIEN 0x02

typedef struct {
    u16 io_base;
    u16 ctrl_base;
    bool master;
    size_t sector_size;
    size_t sector_count;
} ata_device_t;

static void _delay(ata_device_t* dev) {
    if (!dev)
        return;

    for (size_t i = 0; i < 4; i++)
        inb(dev->ctrl_base);
}

static bool _poll(ata_device_t* dev) {
    if (!dev)
        return false;

    _delay(dev);

    for (size_t i = 0; i < 100000; i++) {
        u8 status = inb(dev->io_base + ATA_REG_STATUS);

        if (status & ATA_SR_ERR)
            return false;

        if (status & ATA_SR_DF)
            return false;

        if (!(status & ATA_SR_BUSY) && (status & ATA_SR_DRQ))
            return true;
    }

    return false;
}

static void _select(ata_device_t* dev, u32 lba) {
    u8 head = (u8)((lba >> 24) & 0x0f);
    u8 device = 0xe0 | (dev->master ? 0x00 : 0x10) | head;

    outb(dev->io_base + ATA_REG_DEVICE, device);
    _delay(dev);
}

static bool _identify(ata_device_t* dev, u16* data) {
    if (!dev || !data)
        return false;

    _select(dev, 0);

    outb(dev->io_base + ATA_REG_SECCOUNT, 0);
    outb(dev->io_base + ATA_REG_LBA0, 0);
    outb(dev->io_base + ATA_REG_LBA1, 0);
    outb(dev->io_base + ATA_REG_LBA2, 0);
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (inb(dev->io_base + ATA_REG_STATUS) == 0)
        return false;

    if (!_poll(dev))
        return false;

    for (size_t i = 0; i < 256; i++)
        data[i] = inw(dev->io_base + ATA_REG_DATA);

    return true;
}

static bool _read_sector(ata_device_t* dev, u32 lba, u16* buffer) {
    if (!dev || !buffer)
        return false;

    if (lba > 0x0fffffff)
        return false;

    _select(dev, lba);

    outb(dev->io_base + ATA_REG_SECCOUNT, 1);
    outb(dev->io_base + ATA_REG_LBA0, (u8)(lba & 0xff));
    outb(dev->io_base + ATA_REG_LBA1, (u8)((lba >> 8) & 0xff));
    outb(dev->io_base + ATA_REG_LBA2, (u8)((lba >> 16) & 0xff));
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    if (!_poll(dev))
        return false;

    for (size_t i = 0; i < ATA_SECTOR_SIZE / 2; i++)
        buffer[i] = inw(dev->io_base + ATA_REG_DATA);

    return true;
}

static ssize_t _read(disk_dev_t* dev, void* dest, size_t offset, size_t bytes) {
    if (!dev || !dest || !dev->private)
        return -1;

    ata_device_t* ata = dev->private;
    size_t disk_size = ata->sector_count * ata->sector_size;

    if (offset >= disk_size)
        return 0;

    if (offset + bytes > disk_size)
        bytes = disk_size - offset;

    if (!bytes)
        return 0;

    u8* out = dest;
    size_t lba = offset / ata->sector_size;
    size_t sector_off = offset % ata->sector_size;

    u8* bounce = malloc(ata->sector_size);
    if (!bounce)
        return -1;

    size_t remaining = bytes;

    while (remaining) {
        if (!_read_sector(ata, (u32)lba, (u16*)bounce)) {
            free(bounce);
            return -1;
        }

        size_t available = ata->sector_size - sector_off;
        size_t chunk = remaining < available ? remaining : available;

        memcpy(out, bounce + sector_off, chunk);

        out += chunk;
        remaining -= chunk;
        sector_off = 0;
        lba++;
    }

    free(bounce);
    return (ssize_t)bytes;
}

static ssize_t _write(disk_dev_t* dev, void* src, size_t offset, size_t bytes) {
    (void)dev;
    (void)src;
    (void)offset;
    (void)bytes;
    return -1;
}

static char* _strdup(const char* src) {
    if (!src)
        return NULL;

    size_t len = strlen(src);
    char* out = malloc(len + 1);

    if (!out)
        return NULL;

    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

bool ata_disk_init(void) {
    ata_device_t* ata = calloc(1, sizeof(ata_device_t));
    if (!ata)
        return false;

    ata->io_base = ATA_PRIMARY_BASE;
    ata->ctrl_base = ATA_PRIMARY_CTRL;
    ata->master = true;

    outb(ata->ctrl_base, ATA_CTRL_NIEN);

    u16 identify[256];
    if (!_identify(ata, identify)) {
        log_warn("ata: primary master not present");
        free(ata);
        return false;
    }

    u32 lba28 = (u32)identify[60] | ((u32)identify[61] << 16);
    u64 lba48 = (u64)identify[100] | ((u64)identify[101] << 16) | ((u64)identify[102] << 32) |
                ((u64)identify[103] << 48);

    size_t sectors = lba28 ? (size_t)lba28 : (size_t)lba48;
    if (sectors > 0x0fffffff)
        sectors = 0x0fffffff;

    if (!sectors) {
        log_warn("ata: device reported zero sectors");
        free(ata);
        return false;
    }

    ata->sector_size = ATA_SECTOR_SIZE;
    ata->sector_count = sectors;

    static disk_interface_t ata_interface = {
        .read = _read,
        .write = _write,
    };

    disk_dev_t* disk = calloc(1, sizeof(disk_dev_t));
    if (!disk) {
        free(ata);
        return false;
    }

    disk->name = _strdup("hda");
    disk->type = DISK_HARD;
    disk->interface = &ata_interface;
    disk->sector_size = ata->sector_size;
    disk->sector_count = ata->sector_count;
    disk->private = ata;

    if (!disk->name || !disk_register(disk)) {
        free(disk->name);
        free(disk);
        free(ata);
        return false;
    }

    log_info("ata: primary master ready (%zu sectors)", disk->sector_count);
    return true;
}
