#include "ata.h"

#include <arch/arch.h>
#include <base/types.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdlib.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/irq.h>

#include <sys/disk.h>
#include <sys/pci.h>
#include <sys/time.h>


#define ATA_PRIMARY_BASE   0x1f0
#define ATA_PRIMARY_CTRL   0x3f6
#define ATA_SECONDARY_BASE 0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_SECTOR_SIZE   512
#define ATAPI_SECTOR_SIZE 2048

#define ATA_REG_DATA     0x00
#define ATA_REG_ERROR    0x01
#define ATA_REG_SECCOUNT 0x02
#define ATA_REG_LBA0     0x03
#define ATA_REG_LBA1     0x04
#define ATA_REG_LBA2     0x05
#define ATA_REG_DEVICE   0x06
#define ATA_REG_STATUS   0x07
#define ATA_REG_COMMAND  0x07

#define ATA_CMD_IDENTIFY        0xec
#define ATA_CMD_IDENTIFY_PACKET 0xa1
#define ATA_CMD_PACKET          0xa0
#define ATA_CMD_READ_PIO        0x20
#define ATA_CMD_WRITE_PIO       0x30
#define ATA_CMD_CACHE_FLUSH     0xe7

#define ATAPI_LBA1_SIGNATURE 0x14
#define ATAPI_LBA2_SIGNATURE 0xeb

#define ATA_SR_BUSY  0x80
#define ATA_SR_READY 0x40
#define ATA_SR_DF    0x20
#define ATA_SR_DRQ   0x08
#define ATA_SR_ERR   0x01

#define ATA_CTRL_IRQ_ENABLE 0x00
#define ATA_CTRL_IRQ_DISABLE 0x02
#define ATA_MAX_PIO_SECTORS 255
#define ATA_IRQ_TIMEOUT_MS  100

#define ATA_PCI_BAR0 0x10
#define ATA_PCI_BAR1 0x14
#define ATA_PCI_BAR2 0x18
#define ATA_PCI_BAR3 0x1c

typedef struct {
    u16 io_base;
    u16 ctrl_base;
    volatile bool irq_enabled;
    volatile bool irq_error;
    volatile u64 irq_seq;
    volatile bool io_busy;
    sched_wait_queue_t io_wait;
    sched_wait_queue_t irq_wait;
} ata_channel_t;

typedef struct {
    ata_channel_t *channel;
    bool master;
    bool is_atapi;
    size_t sector_size;
    size_t sector_count;
} ata_device_t;

static ata_channel_t ata_channels[2]; // 0 = primary, 1 = secondary
static bool ata_channel_irq_done[2];
static disk_dev_t *ata_registered_disks[4] = {0};
static bool ata_driver_loaded = false;

const driver_desc_t ata_driver_desc = {
    .name = "ata",
    .deps = NULL,
    .stage = DRIVER_STAGE_STORAGE,
    .load = ata_driver_load,
    .unload = ata_driver_unload,
    .is_busy = ata_driver_busy,
};


static void ata_delay(ata_device_t *dev) {
    if (!dev || !dev->channel) {
        return;
    }

    for (size_t i = 0; i < 4; i++) {
        inb(dev->channel->ctrl_base);
    }
}

static void ata_lock(ata_device_t *dev) {
    if (!dev || !dev->channel) {
        return;
    }

    ata_channel_t *ch = dev->channel;

    for (;;) {
        unsigned long flags = arch_irq_save();

        if (!ch->io_busy) {
            ch->io_busy = true;
            arch_irq_restore(flags);
            return;
        }

        arch_irq_restore(flags);

        if (sched_is_running() && sched_current() && ch->io_wait.list) {
            sched_block(&ch->io_wait);
            continue;
        }

        arch_cpu_wait();
    }
}

static void ata_unlock(ata_device_t *dev) {
    if (!dev || !dev->channel) {
        return;
    }

    ata_channel_t *ch = dev->channel;
    unsigned long flags = arch_irq_save();

    ch->io_busy = false;

    arch_irq_restore(flags);

    if (ch->io_wait.list) {
        sched_wake_one(&ch->io_wait);
    }
}

static u64 ata_irq_snapshot(ata_device_t *dev) {
    if (!dev || !dev->channel) {
        return 0;
    }

    ata_channel_t *ch = dev->channel;
    unsigned long flags = arch_irq_save();

    u64 seq = ch->irq_seq;
    ch->irq_error = false;

    arch_irq_restore(flags);

    return seq;
}

static bool ata_wait_irq_event(ata_device_t *dev, u64 *seq) {
    if (!dev || !dev->channel || !seq) {
        return false;
    }

    ata_channel_t *ch = dev->channel;
    u64 start = arch_timer_ticks();
    u64 timeout = ms_to_ticks(ATA_IRQ_TIMEOUT_MS);

    for (;;) {
        unsigned long flags = arch_irq_save();
        u64 now = ch->irq_seq;
        bool had_error = ch->irq_error;

        if (now != *seq) {
            *seq = now;
            ch->irq_error = false;
            arch_irq_restore(flags);
            return !had_error;
        }

        arch_irq_restore(flags);

        if ((arch_timer_ticks() - start) >= timeout) {
            return false;
        }

        if (sched_is_running() && sched_current()) {
            sched_thread_t *current = sched_current();
            if (current && sched_signal_has_pending(current)) {
                return false;
            }

            sched_yield();
            continue;
        }

        arch_cpu_wait();
    }
}

static bool ata_poll(ata_device_t *dev) {
    if (!dev || !dev->channel) {
        return false;
    }

    ata_delay(dev);

    for (size_t i = 0; i < 100000; i++) {
        u8 status = inb(dev->channel->io_base + ATA_REG_STATUS);

        if (status & ATA_SR_ERR) {
            return false;
        }

        if (status & ATA_SR_DF) {
            return false;
        }

        if (!(status & ATA_SR_BUSY) && (status & ATA_SR_DRQ)) {
            return true;
        }

        cpu_pause();
    }

    return false;
}

static bool ata_wait_ready(ata_device_t *dev) {
    if (!dev || !dev->channel) {
        return false;
    }

    ata_delay(dev);

    for (size_t i = 0; i < 100000; i++) {
        u8 status = inb(dev->channel->io_base + ATA_REG_STATUS);

        if (status & ATA_SR_ERR) {
            return false;
        }

        if (status & ATA_SR_DF) {
            return false;
        }

        if (!(status & ATA_SR_BUSY) && (status & ATA_SR_READY)) {
            return true;
        }

        cpu_pause();
    }

    return false;
}

static bool ata_wait_drq(ata_device_t *dev, u64 *seq) {
    if (!dev || !dev->channel) {
        return false;
    }

    if (!dev->channel->irq_enabled) {
        return ata_poll(dev);
    }

    for (;;) {
        u8 status = inb(dev->channel->io_base + ATA_REG_STATUS);

        if (status & ATA_SR_ERR) {
            return false;
        }

        if (status & ATA_SR_DF) {
            return false;
        }

        if (!(status & ATA_SR_BUSY) && (status & ATA_SR_DRQ)) {
            return true;
        }

        if (!ata_wait_irq_event(dev, seq)) {
            return false;
        }
    }
}

static bool ata_wait_ready_event(ata_device_t *dev, u64 *seq) {
    if (!dev || !dev->channel) {
        return false;
    }

    if (!dev->channel->irq_enabled) {
        return ata_wait_ready(dev);
    }

    for (;;) {
        u8 status = inb(dev->channel->io_base + ATA_REG_STATUS);

        if (status & ATA_SR_ERR) {
            return false;
        }

        if (status & ATA_SR_DF) {
            return false;
        }

        if (!(status & ATA_SR_BUSY) && (status & ATA_SR_READY)) {
            return true;
        }

        if (!ata_wait_irq_event(dev, seq)) {
            return false;
        }
    }
}

static void ata_primary_irq(UNUSED int_state_t *s) {
    ata_channel_t *ch = &ata_channels[0];

    u8 status = inb(ch->io_base + ATA_REG_STATUS);

    unsigned long flags = arch_irq_save();

    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        ch->irq_error = true;
    }

    ch->irq_seq++;
    arch_irq_restore(flags);

    if (ch->irq_wait.list) {
        sched_wake_all(&ch->irq_wait);
    }

    irq_ack(IRQ_PRIMARY_ATA);
}

static void ata_secondary_irq(UNUSED int_state_t *s) {
    ata_channel_t *ch = &ata_channels[1];

    u8 status = inb(ch->io_base + ATA_REG_STATUS);

    unsigned long flags = arch_irq_save();

    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        ch->irq_error = true;
    }

    ch->irq_seq++;
    arch_irq_restore(flags);

    if (ch->irq_wait.list) {
        sched_wake_all(&ch->irq_wait);
    }

    irq_ack(IRQ_SECONDARY_ATA);
}

static void ata_select(ata_device_t *dev, u32 lba) {
    u8 head = (u8)((lba >> 24) & 0x0f);
    u8 device = 0xe0 | (dev->master ? 0x00 : 0x10) | head;

    outb(dev->channel->io_base + ATA_REG_DEVICE, device);
    ata_delay(dev);
}

static bool ata_identify(ata_device_t *dev, u16 *data) {
    if (!dev || !data) {
        return false;
    }

    ata_select(dev, 0);

    outb(dev->channel->io_base + ATA_REG_SECCOUNT, 0);
    outb(dev->channel->io_base + ATA_REG_LBA0, 0);
    outb(dev->channel->io_base + ATA_REG_LBA1, 0);
    outb(dev->channel->io_base + ATA_REG_LBA2, 0);
    outb(dev->channel->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (!inb(dev->channel->io_base + ATA_REG_STATUS)) {
        return false;
    }

    if (!ata_poll(dev)) {
        return false;
    }

    for (size_t i = 0; i < 256; i++) {
        data[i] = inw(dev->channel->io_base + ATA_REG_DATA);
    }

    return true;
}

static bool ata_identify_packet(ata_device_t *dev, u16 *data) {
    if (!dev || !data) {
        return false;
    }

    ata_select(dev, 0);

    outb(dev->channel->io_base + ATA_REG_SECCOUNT, 0);
    outb(dev->channel->io_base + ATA_REG_LBA0, 0);
    outb(dev->channel->io_base + ATA_REG_LBA1, 0);
    outb(dev->channel->io_base + ATA_REG_LBA2, 0);
    outb(dev->channel->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);

    if (!inb(dev->channel->io_base + ATA_REG_STATUS)) {
        return false;
    }

    if (!ata_poll(dev)) {
        return false;
    }

    for (size_t i = 0; i < 256; i++) {
        data[i] = inw(dev->channel->io_base + ATA_REG_DATA);
    }

    return true;
}

static bool
atapi_read_sectors(ata_device_t *dev, u32 lba, u16 count, void *buffer) {
    if (!dev || !buffer || !count) {
        return false;
    }

    u8 *out = buffer;

    for (u16 s = 0; s < count; s++) {
        u32 cur_lba = lba + s;

        ata_select(dev, 0);

        outb(dev->channel->io_base + ATA_REG_ERROR, 0);
        outb(
            dev->channel->io_base + ATA_REG_LBA1, (u8)(ATAPI_SECTOR_SIZE & 0xff)
        );
        outb(
            dev->channel->io_base + ATA_REG_LBA2, (u8)(ATAPI_SECTOR_SIZE >> 8)
        );

        u64 seq = ata_irq_snapshot(dev);

        outb(dev->channel->io_base + ATA_REG_COMMAND, ATA_CMD_PACKET);

        // CDB phase: the device sets DRQ when ready for the command packet
        // This transition does NOT always assert INTRQ, so we poll for it
        if (!ata_poll(dev)) {
            return false;
        }

        // READ(12) SCSI CDB
        u8 cdb[12] = {0};
        cdb[0] = 0xa8; // READ(12) opcode
        cdb[2] = (u8)((cur_lba >> 24) & 0xff);
        cdb[3] = (u8)((cur_lba >> 16) & 0xff);
        cdb[4] = (u8)((cur_lba >> 8) & 0xff);
        cdb[5] = (u8)(cur_lba & 0xff);
        cdb[9] = 1; // transfer 1 sector

        for (size_t i = 0; i < 6; i++) {
            outw(
                dev->channel->io_base + ATA_REG_DATA,
                (u16)cdb[i * 2] | ((u16)cdb[i * 2 + 1] << 8)
            );
        }

        // Data phase: the device asserts INTRQ when data is ready
        if (!ata_wait_drq(dev, &seq)) {
            return false;
        }

        u16 *dst = (u16 *)(out + (size_t)s * ATAPI_SECTOR_SIZE);

        for (size_t i = 0; i < ATAPI_SECTOR_SIZE / 2; i++) {
            dst[i] = inw(dev->channel->io_base + ATA_REG_DATA);
        }
    }

    return true;
}

static bool
ata_read_sectors(ata_device_t *dev, u32 lba, u8 count, u16 *buffer) {
    if (!dev || !buffer || !count) {
        return false;
    }

    if ((u64)lba + (u64)count - 1 > 0x0fffffffULL) {
        return false;
    }

    ata_select(dev, lba);

    outb(dev->channel->io_base + ATA_REG_SECCOUNT, count);
    outb(dev->channel->io_base + ATA_REG_LBA0, (u8)(lba & 0xff));
    outb(dev->channel->io_base + ATA_REG_LBA1, (u8)((lba >> 8) & 0xff));
    outb(dev->channel->io_base + ATA_REG_LBA2, (u8)((lba >> 16) & 0xff));

    u64 seq = ata_irq_snapshot(dev);

    outb(dev->channel->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    for (u32 sector = 0; sector < count; sector++) {
        if (!ata_wait_drq(dev, &seq)) {
            return false;
        }

        u16 *dst = buffer + sector * (ATA_SECTOR_SIZE / 2);

        for (size_t i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
            dst[i] = inw(dev->channel->io_base + ATA_REG_DATA);
        }
    }

    return true;
}

static bool
ata_write_sectors(ata_device_t *dev, u32 lba, u8 count, const u16 *buffer) {
    if (!dev || !buffer || !count) {
        return false;
    }

    if ((u64)lba + (u64)count - 1 > 0x0fffffffULL) {
        return false;
    }

    ata_select(dev, lba);

    outb(dev->channel->io_base + ATA_REG_SECCOUNT, count);
    outb(dev->channel->io_base + ATA_REG_LBA0, (u8)(lba & 0xff));
    outb(dev->channel->io_base + ATA_REG_LBA1, (u8)((lba >> 8) & 0xff));
    outb(dev->channel->io_base + ATA_REG_LBA2, (u8)((lba >> 16) & 0xff));

    u64 seq = ata_irq_snapshot(dev);

    outb(dev->channel->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    for (u32 sector = 0; sector < count; sector++) {
        if (!ata_wait_drq(dev, &seq)) {
            return false;
        }

        const u16 *src = buffer + sector * (ATA_SECTOR_SIZE / 2);

        for (size_t i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
            outw(dev->channel->io_base + ATA_REG_DATA, src[i]);
        }
    }

    if (!ata_wait_ready_event(dev, &seq)) {
        return false;
    }

    outb(dev->channel->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_ready_event(dev, &seq);
}

static ssize_t
ata_read(disk_dev_t *dev, void *dest, size_t offset, size_t bytes) {
    if (!dev || !dest || !dev->private) {
        return -1;
    }

    ata_device_t *ata = dev->private;
    ata_lock(ata);

    ssize_t ret = -1;
    size_t disk_size = ata->sector_count * ata->sector_size;

    if (offset >= disk_size) {
        goto done_empty;
    }

    if (offset + bytes > disk_size) {
        bytes = disk_size - offset;
    }

    if (!bytes) {
        goto done_empty;
    }

    u8 *out = dest;

    if (ata->is_atapi) {
        u8 *bounce = malloc(ATAPI_SECTOR_SIZE);
        if (!bounce) {
            goto done;
        }

        size_t remaining = bytes;

        while (remaining > 0) {
            u32 atapi_lba = (u32)(offset / ATAPI_SECTOR_SIZE);
            size_t sector_off = offset % ATAPI_SECTOR_SIZE;

            if (!atapi_read_sectors(ata, atapi_lba, 1, bounce)) {
                free(bounce);
                goto done;
            }

            size_t available = ATAPI_SECTOR_SIZE - sector_off;
            size_t chunk = remaining < available ? remaining : available;

            memcpy(out, bounce + sector_off, chunk);

            out += chunk;
            remaining -= chunk;
            offset += chunk;
        }

        free(bounce);
        ret = (ssize_t)bytes;
        goto done;
    }

    size_t lba = offset / ata->sector_size;
    size_t sector_off = offset % ata->sector_size;

    u8 *bounce = malloc(ata->sector_size);
    if (!bounce) {
        goto done;
    }

    size_t remaining = bytes;

    if (sector_off) {
        if (!ata_read_sectors(ata, (u32)lba, 1, (u16 *)bounce)) {
            goto done_free;
        }

        size_t available = ata->sector_size - sector_off;
        size_t chunk = remaining < available ? remaining : available;

        memcpy(out, bounce + sector_off, chunk);

        out += chunk;
        remaining -= chunk;
        lba++;
    }

    while (remaining >= ata->sector_size) {
        size_t full_sectors = remaining / ata->sector_size;
        size_t batch = full_sectors;

        if (batch > ATA_MAX_PIO_SECTORS) {
            batch = ATA_MAX_PIO_SECTORS;
        }

        if (!ata_read_sectors(ata, (u32)lba, (u8)batch, (u16 *)out)) {
            goto done_free;
        }

        size_t batch_bytes = batch * ata->sector_size;

        out += batch_bytes;
        remaining -= batch_bytes;
        lba += batch;
    }

    if (remaining) {
        if (!ata_read_sectors(ata, (u32)lba, 1, (u16 *)bounce)) {
            goto done_free;
        }

        memcpy(out, bounce, remaining);
    }

    ret = (ssize_t)bytes;

done_free:
    free(bounce);

done:
    ata_unlock(ata);
    return ret;

done_empty:
    ret = 0;
    goto done;
}

static ssize_t
ata_write(disk_dev_t *dev, void *src, size_t offset, size_t bytes) {
    if (!dev || !src || !dev->private) {
        return -1;
    }

    ata_device_t *ata = dev->private;

    if (ata->is_atapi) {
        return -1; // optical media is read-only (TODO: wait is it?)
    }

    ata_lock(ata);

    ssize_t ret = -1;
    size_t disk_size = ata->sector_count * ata->sector_size;

    if (offset >= disk_size) {
        goto done_empty;
    }

    if (offset + bytes > disk_size) {
        bytes = disk_size - offset;
    }

    if (!bytes) {
        goto done_empty;
    }

    const u8 *in = src;
    size_t lba = offset / ata->sector_size;
    size_t sector_off = offset % ata->sector_size;

    u8 *bounce = malloc(ata->sector_size);
    if (!bounce) {
        goto done;
    }

    size_t remaining = bytes;

    if (sector_off) {
        size_t available = ata->sector_size - sector_off;
        size_t chunk = remaining < available ? remaining : available;

        if (!ata_read_sectors(ata, (u32)lba, 1, (u16 *)bounce)) {
            goto done_free;
        }

        memcpy(bounce + sector_off, in, chunk);

        if (!ata_write_sectors(ata, (u32)lba, 1, (const u16 *)bounce)) {
            goto done_free;
        }

        in += chunk;
        remaining -= chunk;
        lba++;
    }

    while (remaining >= ata->sector_size) {
        size_t full_sectors = remaining / ata->sector_size;
        size_t batch = full_sectors;

        if (batch > ATA_MAX_PIO_SECTORS) {
            batch = ATA_MAX_PIO_SECTORS;
        }

        if (!ata_write_sectors(ata, (u32)lba, (u8)batch, (const u16 *)in)) {
            goto done_free;
        }

        size_t batch_bytes = batch * ata->sector_size;

        in += batch_bytes;
        remaining -= batch_bytes;
        lba += batch;
    }

    if (remaining) {
        if (!ata_read_sectors(ata, (u32)lba, 1, (u16 *)bounce)) {
            goto done_free;
        }

        memcpy(bounce, in, remaining);

        if (!ata_write_sectors(ata, (u32)lba, 1, (const u16 *)bounce)) {
            goto done_free;
        }
    }

    ret = (ssize_t)bytes;

done_free:
    free(bounce);

done:
    ata_unlock(ata);
    return ret;

done_empty:
    ret = 0;
    goto done;
}

static bool ata_channel_present(u16 io_base) {
    u8 status = inb(io_base + ATA_REG_STATUS);
    return status != 0xff;
}

static const char *ata_disk_names[] = {"hda", "hdb", "hdc", "hdd"};
static const char *atapi_disk_names[] = {"cda", "cdb", "cdc", "cdd"};
static const char *ata_pos_names[] =
    {"primary master", "primary slave", "secondary master", "secondary slave"};

static bool
ata_probe_device(ata_channel_t *ch, bool is_master, size_t dev_index) {
    ata_device_t *ata = calloc(1, sizeof(ata_device_t));
    if (!ata) {
        return false;
    }

    ata->channel = ch;
    ata->master = is_master;
    ata->is_atapi = false;

    u16 identify[256];
    bool found_ata = ata_identify(ata, identify);
    bool found_atapi = false;

    if (!found_ata) {
        u8 lba1 = inb(ch->io_base + ATA_REG_LBA1);
        u8 lba2 = inb(ch->io_base + ATA_REG_LBA2);

        if (lba1 == ATAPI_LBA1_SIGNATURE && lba2 == ATAPI_LBA2_SIGNATURE) {
            found_atapi = ata_identify_packet(ata, identify);
        }
    }

    if (!found_ata && !found_atapi) {
        free(ata);
        return false;
    }

    if (found_atapi) {
        ata->is_atapi = true;
        ata->sector_size = ATAPI_SECTOR_SIZE;

        u32 max_lba = (u32)identify[100] | ((u32)identify[101] << 16);

        if (!max_lba) {
            max_lba = 340000;
        }

        ata->sector_count = max_lba;
    } else {
        ata->sector_size = ATA_SECTOR_SIZE;

        u32 lba28 = (u32)identify[60] | ((u32)identify[61] << 16);
        u64 lba48 = (u64)identify[100] | ((u64)identify[101] << 16) |
                    ((u64)identify[102] << 32) | ((u64)identify[103] << 48);

        size_t sectors = lba28 ? (size_t)lba28 : (size_t)lba48;
        if (sectors > 0x0fffffff) {
            sectors = 0x0fffffff;
        }

        if (!sectors) {
            free(ata);
            return false;
        }

        ata->sector_count = sectors;
    }

    static disk_interface_t ata_interface = {
        .read = ata_read,
        .write = ata_write,
    };

    disk_dev_t *disk = calloc(1, sizeof(disk_dev_t));
    if (!disk) {
        free(ata);
        return false;
    }

    if (found_atapi) {
        disk->name = strdup(atapi_disk_names[dev_index]);
        disk->type = DISK_OPTICAL;
        disk->sector_size = ATA_SECTOR_SIZE;
        disk->sector_count =
            ata->sector_count * (ATAPI_SECTOR_SIZE / ATA_SECTOR_SIZE);
    } else {
        disk->name = strdup(ata_disk_names[dev_index]);
        disk->type = DISK_HARD;
        disk->sector_size = ata->sector_size;
        disk->sector_count = ata->sector_count;
    }

    disk->interface = &ata_interface;
    disk->private = ata;

    if (!disk->name || !disk_register(disk)) {
        free(disk->name);
        free(disk);
        free(ata);
        return false;
    }

    if (found_atapi) {
        log_info("%s: ATAPI CD-ROM", ata_pos_names[dev_index]);
    } else {
        log_info(
            "%s ready (%zu sectors)",
            ata_pos_names[dev_index],
            disk->sector_count
        );
    }

    if (dev_index < (sizeof(ata_registered_disks) / sizeof(ata_registered_disks[0]))) {
        ata_registered_disks[dev_index] = disk;
    }

    return true;
}

static bool
ata_probe_channel(u16 io_base, u16 ctrl_base, bool is_primary, bool use_irq) {
    if (!ata_channel_present(io_base)) {
        return false;
    }

    size_t ch_index = is_primary ? 0 : 1;
    ata_channel_t *ch = &ata_channels[ch_index];

    ch->io_base = io_base;
    ch->ctrl_base = ctrl_base;
    ch->irq_enabled = use_irq;
    ch->irq_error = false;
    ch->irq_seq = 0;
    ch->io_busy = false;

    sched_wait_queue_init(&ch->io_wait);
    sched_wait_queue_init(&ch->irq_wait);

    outb(ctrl_base, ATA_CTRL_IRQ_ENABLE);

    if (use_irq && !ata_channel_irq_done[ch_index]) {
        if (is_primary) {
            irq_register(IRQ_PRIMARY_ATA, ata_primary_irq);
        } else {
            irq_register(IRQ_SECONDARY_ATA, ata_secondary_irq);
        }
        ata_channel_irq_done[ch_index] = true;
    }

    // 0 = primary master, 1 = primary slave, 2 = secondary master, 3 =
    // secondary slave
    size_t master_index = is_primary ? 0 : 2;
    size_t slave_index = master_index + 1;

    bool found_master = ata_probe_device(ch, true, master_index);
    bool found_slave = ata_probe_device(ch, false, slave_index);

    return found_master || found_slave;
}

static u16 ata_pci_read_io_bar(u8 bus, u8 slot, u8 func, u16 offset) {
    u32 bar = pci_read_config(bus, slot, func, offset, 4);

    if (!bar || bar == 0xffffffffU) {
        return 0;
    }

    if (!(bar & 1U)) {
        return 0;
    }

    return (u16)(bar & ~0x3U);
}

static bool ata_probe_pci_ide(void) {
    bool found = false;
    pci_found_t *cursor = NULL;

    for (;;) {
        pci_found_t *node = pci_find_node(PCI_MASS_STORAGE, PCI_MS_IDE, cursor);
        if (!node) {
            break;
        }

        cursor = node;

        u8 prog_if = node->header.prog_if;
        bool pri_native = (prog_if & 0x01) != 0;
        bool sec_native = (prog_if & 0x04) != 0;

        u16 pri_io = ATA_PRIMARY_BASE;
        u16 pri_ctrl = ATA_PRIMARY_CTRL;
        u16 sec_io = ATA_SECONDARY_BASE;
        u16 sec_ctrl = ATA_SECONDARY_CTRL;

        if (pri_native) {
            u16 bar0 = ata_pci_read_io_bar(
                node->bus, node->slot, node->func, ATA_PCI_BAR0
            );
            u16 bar1 = ata_pci_read_io_bar(
                node->bus, node->slot, node->func, ATA_PCI_BAR1
            );

            if (bar0) {
                pri_io = bar0;
            }

            if (bar1) {
                pri_ctrl = (u16)(bar1 + 2);
            }
        }

        if (sec_native) {
            u16 bar2 = ata_pci_read_io_bar(
                node->bus, node->slot, node->func, ATA_PCI_BAR2
            );
            u16 bar3 = ata_pci_read_io_bar(
                node->bus, node->slot, node->func, ATA_PCI_BAR3
            );

            if (bar2) {
                sec_io = bar2;
            }

            if (bar3) {
                sec_ctrl = (u16)(bar3 + 2);
            }
        }

        pci_enable_bus_mastering(node->bus, node->slot, node->func);

        log_debug(
            "IDE controller %u:%u.%u prog_if=%#x pri=%#x/%#x sec=%#x/%#x",
            node->bus,
            node->slot,
            node->func,
            prog_if,
            pri_io,
            pri_ctrl,
            sec_io,
            sec_ctrl
        );

        if (ata_probe_channel(pri_io, pri_ctrl, true, !pri_native)) {
            found = true;
        }

        if (ata_probe_channel(sec_io, sec_ctrl, false, !sec_native)) {
            found = true;
        }

        if (found) {
            return true;
        }
    }

    return false;
}

static bool ata_disk_init(void) {
    bool found = ata_probe_pci_ide();

    if (!found && ata_probe_channel(ATA_PRIMARY_BASE, ATA_PRIMARY_CTRL, true, true)) {
        found = true;
    }

    if (!found && ata_probe_channel(ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, false, true)) {
        found = true;
    }

    if (!found) {
        log_warn("no ata devices found");
        return false;
    }

    return true;
}

bool ata_driver_busy(void) {
    for (size_t i = 0; i < (sizeof(ata_registered_disks) / sizeof(ata_registered_disks[0])); i++) {
        disk_dev_t *disk = ata_registered_disks[i];
        if (disk && disk_is_busy(disk)) {
            return true;
        }
    }

    return false;
}

driver_err_t ata_driver_load(void) {
    if (ata_driver_loaded) {
        return DRIVER_OK;
    }

    if (!ata_disk_init()) {
        return DRIVER_ERR_INIT_FAILED;
    }

    ata_driver_loaded = true;
    return DRIVER_OK;
}

driver_err_t ata_driver_unload(void) {
    if (!ata_driver_loaded) {
        return DRIVER_OK;
    }

    if (ata_driver_busy()) {
        return DRIVER_ERR_BUSY;
    }

    for (size_t i = 0; i < (sizeof(ata_registered_disks) / sizeof(ata_registered_disks[0])); i++) {
        disk_dev_t *disk = ata_registered_disks[i];
        if (!disk) {
            continue;
        }

        if (!disk_unregister(disk)) {
            return DRIVER_ERR_BUSY;
        }
    }

    for (size_t i = 0; i < (sizeof(ata_registered_disks) / sizeof(ata_registered_disks[0])); i++) {
        disk_dev_t *disk = ata_registered_disks[i];
        if (!disk) {
            continue;
        }

        ata_device_t *ata = disk->private;
        free(disk->name);
        free(disk);
        free(ata);
        ata_registered_disks[i] = NULL;
    }

    for (size_t i = 0; i < 2; i++) {
        ata_channel_t *channel = &ata_channels[i];
        if (channel->ctrl_base) {
            outb(channel->ctrl_base, ATA_CTRL_IRQ_DISABLE);
        }
    }

    if (ata_channel_irq_done[0]) {
        irq_unregister(IRQ_PRIMARY_ATA);
    }
    if (ata_channel_irq_done[1]) {
        irq_unregister(IRQ_SECONDARY_ATA);
    }

    memset(ata_channels, 0, sizeof(ata_channels));
    memset(ata_channel_irq_done, 0, sizeof(ata_channel_irq_done));
    ata_driver_loaded = false;

    return DRIVER_OK;
}
