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

#define ATA_CMD_IDENTIFY    0xec
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xe7

#define ATA_SR_BUSY  0x80
#define ATA_SR_READY 0x40
#define ATA_SR_DF    0x20
#define ATA_SR_DRQ   0x08
#define ATA_SR_ERR   0x01

#define ATA_CTRL_IRQ_ENABLE 0x00
#define ATA_MAX_PIO_SECTORS 255
#define ATA_IRQ_TIMEOUT_MS 100

typedef struct {
    u16 io_base;
    u16 ctrl_base;
    bool master;
    size_t sector_size;
    size_t sector_count;
    volatile bool irq_enabled;
    volatile bool irq_error;
    volatile u64 irq_seq;
    volatile bool io_busy;
    sched_wait_queue_t io_wait;
    sched_wait_queue_t irq_wait;
} ata_device_t;

static ata_device_t* ata_primary = NULL;

static u64 ata_irq_timeout_ticks(void) {
    u32 hz = arch_timer_hz();
    if (!hz)
        return 1;

    u64 ticks = ((u64)hz * ATA_IRQ_TIMEOUT_MS + 999ULL) / 1000ULL;
    return ticks ? ticks : 1;
}

static void ata_delay(ata_device_t* dev) {
    if (!dev)
        return;

    for (size_t i = 0; i < 4; i++)
        inb(dev->ctrl_base);
}

static void ata_lock(ata_device_t* dev) {
    if (!dev)
        return;

    for (;;) {
        unsigned long flags = arch_irq_save();

        if (!dev->io_busy) {
            dev->io_busy = true;
            arch_irq_restore(flags);
            return;
        }

        arch_irq_restore(flags);

        if (sched_is_running() && sched_current() && dev->io_wait.list) {
            sched_block(&dev->io_wait);
            continue;
        }

        arch_cpu_wait();
    }
}

static void ata_unlock(ata_device_t* dev) {
    if (!dev)
        return;

    unsigned long flags = arch_irq_save();

    dev->io_busy = false;

    arch_irq_restore(flags);

    if (dev->io_wait.list)
        sched_wake_one(&dev->io_wait);
}

static u64 ata_irq_snapshot(ata_device_t* dev) {
    if (!dev)
        return 0;

    unsigned long flags = arch_irq_save();

    u64 seq = dev->irq_seq;
    dev->irq_error = false;

    arch_irq_restore(flags);

    return seq;
}

static bool ata_wait_irq_event(ata_device_t* dev, u64* seq) {
    if (!dev || !seq)
        return false;

    u64 start = arch_timer_ticks();
    u64 timeout = ata_irq_timeout_ticks();

    for (;;) {
        unsigned long flags = arch_irq_save();
        u64 now = dev->irq_seq;
        bool had_error = dev->irq_error;

        if (now != *seq) {
            *seq = now;
            dev->irq_error = false;
            arch_irq_restore(flags);
            return !had_error;
        }

        arch_irq_restore(flags);

        if ((arch_timer_ticks() - start) >= timeout)
            return false;

        if (sched_is_running() && sched_current()) {
            sched_thread_t* current = sched_current();
            if (current && sched_signal_has_pending(current))
                return false;

            sched_yield();
            continue;
        }

        arch_cpu_wait();
    }
}

static bool ata_poll(ata_device_t* dev) {
    if (!dev)
        return false;

    ata_delay(dev);

    for (size_t i = 0; i < 100000; i++) {
        u8 status = inb(dev->io_base + ATA_REG_STATUS);

        if (status & ATA_SR_ERR)
            return false;

        if (status & ATA_SR_DF)
            return false;

        if (!(status & ATA_SR_BUSY) && (status & ATA_SR_DRQ))
            return true;

        cpu_pause();
    }

    return false;
}

static bool ata_wait_ready(ata_device_t* dev) {
    if (!dev)
        return false;

    ata_delay(dev);

    for (size_t i = 0; i < 100000; i++) {
        u8 status = inb(dev->io_base + ATA_REG_STATUS);

        if (status & ATA_SR_ERR)
            return false;

        if (status & ATA_SR_DF)
            return false;

        if (!(status & ATA_SR_BUSY) && (status & ATA_SR_READY))
            return true;

        cpu_pause();
    }

    return false;
}

static bool ata_wait_drq(ata_device_t* dev, u64* seq) {
    if (!dev)
        return false;

    if (!dev->irq_enabled)
        return ata_poll(dev);

    for (;;) {
        u8 status = inb(dev->io_base + ATA_REG_STATUS);

        if (status & ATA_SR_ERR)
            return false;

        if (status & ATA_SR_DF)
            return false;

        if (!(status & ATA_SR_BUSY) && (status & ATA_SR_DRQ))
            return true;

        if (!ata_wait_irq_event(dev, seq))
            return false;
    }
}

static bool ata_wait_ready_event(ata_device_t* dev, u64* seq) {
    if (!dev)
        return false;

    if (!dev->irq_enabled)
        return ata_wait_ready(dev);

    for (;;) {
        u8 status = inb(dev->io_base + ATA_REG_STATUS);

        if (status & ATA_SR_ERR)
            return false;

        if (status & ATA_SR_DF)
            return false;

        if (!(status & ATA_SR_BUSY) && (status & ATA_SR_READY))
            return true;

        if (!ata_wait_irq_event(dev, seq))
            return false;
    }
}

static void ata_primary_irq(UNUSED int_state_t* s) {
    if (!ata_primary) {
        irq_ack(IRQ_PRIMARY_ATA);
        return;
    }

    u8 status = inb(ata_primary->io_base + ATA_REG_STATUS);

    unsigned long flags = arch_irq_save();

    if (status & (ATA_SR_ERR | ATA_SR_DF))
        ata_primary->irq_error = true;

    ata_primary->irq_seq++;
    arch_irq_restore(flags);

    if (ata_primary->irq_wait.list)
        sched_wake_all(&ata_primary->irq_wait);

    irq_ack(IRQ_PRIMARY_ATA);
}

static void ata_select(ata_device_t* dev, u32 lba) {
    u8 head = (u8)((lba >> 24) & 0x0f);
    u8 device = 0xe0 | (dev->master ? 0x00 : 0x10) | head;

    outb(dev->io_base + ATA_REG_DEVICE, device);
    ata_delay(dev);
}

static bool ata_identify(ata_device_t* dev, u16* data) {
    if (!dev || !data)
        return false;

    ata_select(dev, 0);

    outb(dev->io_base + ATA_REG_SECCOUNT, 0);
    outb(dev->io_base + ATA_REG_LBA0, 0);
    outb(dev->io_base + ATA_REG_LBA1, 0);
    outb(dev->io_base + ATA_REG_LBA2, 0);
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (!inb(dev->io_base + ATA_REG_STATUS))
        return false;

    if (!ata_poll(dev))
        return false;

    for (size_t i = 0; i < 256; i++)
        data[i] = inw(dev->io_base + ATA_REG_DATA);

    return true;
}

static bool ata_read_sectors(ata_device_t* dev, u32 lba, u8 count, u16* buffer) {
    if (!dev || !buffer || !count)
        return false;

    if ((u64)lba + (u64)count - 1 > 0x0fffffffULL)
        return false;

    ata_select(dev, lba);

    outb(dev->io_base + ATA_REG_SECCOUNT, count);
    outb(dev->io_base + ATA_REG_LBA0, (u8)(lba & 0xff));
    outb(dev->io_base + ATA_REG_LBA1, (u8)((lba >> 8) & 0xff));
    outb(dev->io_base + ATA_REG_LBA2, (u8)((lba >> 16) & 0xff));

    u64 seq = ata_irq_snapshot(dev);

    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    for (u32 sector = 0; sector < count; sector++) {
        if (!ata_wait_drq(dev, &seq))
            return false;

        u16* dst = buffer + sector * (ATA_SECTOR_SIZE / 2);

        for (size_t i = 0; i < ATA_SECTOR_SIZE / 2; i++)
            dst[i] = inw(dev->io_base + ATA_REG_DATA);
    }

    return true;
}

static bool ata_write_sectors(ata_device_t* dev, u32 lba, u8 count, const u16* buffer) {
    if (!dev || !buffer || !count)
        return false;

    if ((u64)lba + (u64)count - 1 > 0x0fffffffULL)
        return false;

    ata_select(dev, lba);

    outb(dev->io_base + ATA_REG_SECCOUNT, count);
    outb(dev->io_base + ATA_REG_LBA0, (u8)(lba & 0xff));
    outb(dev->io_base + ATA_REG_LBA1, (u8)((lba >> 8) & 0xff));
    outb(dev->io_base + ATA_REG_LBA2, (u8)((lba >> 16) & 0xff));

    u64 seq = ata_irq_snapshot(dev);

    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    for (u32 sector = 0; sector < count; sector++) {
        if (!ata_wait_drq(dev, &seq))
            return false;

        const u16* src = buffer + sector * (ATA_SECTOR_SIZE / 2);

        for (size_t i = 0; i < ATA_SECTOR_SIZE / 2; i++)
            outw(dev->io_base + ATA_REG_DATA, src[i]);
    }

    if (!ata_wait_ready_event(dev, &seq))
        return false;

    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_ready_event(dev, &seq);
}

static ssize_t ata_read(disk_dev_t* dev, void* dest, size_t offset, size_t bytes) {
    if (!dev || !dest || !dev->private)
        return -1;

    ata_device_t* ata = dev->private;
    ata_lock(ata);

    ssize_t ret = -1;
    size_t disk_size = ata->sector_count * ata->sector_size;

    if (offset >= disk_size)
        goto done_empty;

    if (offset + bytes > disk_size)
        bytes = disk_size - offset;

    if (!bytes)
        goto done_empty;

    u8* out = dest;
    size_t lba = offset / ata->sector_size;
    size_t sector_off = offset % ata->sector_size;

    u8* bounce = malloc(ata->sector_size);
    if (!bounce)
        goto done;

    size_t remaining = bytes;

    if (sector_off) {
        if (!ata_read_sectors(ata, (u32)lba, 1, (u16*)bounce))
            goto done_free;

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

        if (batch > ATA_MAX_PIO_SECTORS)
            batch = ATA_MAX_PIO_SECTORS;

        if (!ata_read_sectors(ata, (u32)lba, (u8)batch, (u16*)out))
            goto done_free;

        size_t batch_bytes = batch * ata->sector_size;

        out += batch_bytes;
        remaining -= batch_bytes;
        lba += batch;
    }

    if (remaining) {
        if (!ata_read_sectors(ata, (u32)lba, 1, (u16*)bounce))
            goto done_free;

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

static ssize_t ata_write(disk_dev_t* dev, void* src, size_t offset, size_t bytes) {
    if (!dev || !src || !dev->private)
        return -1;

    ata_device_t* ata = dev->private;
    ata_lock(ata);

    ssize_t ret = -1;
    size_t disk_size = ata->sector_count * ata->sector_size;

    if (offset >= disk_size)
        goto done_empty;

    if (offset + bytes > disk_size)
        bytes = disk_size - offset;

    if (!bytes)
        goto done_empty;

    const u8* in = src;
    size_t lba = offset / ata->sector_size;
    size_t sector_off = offset % ata->sector_size;

    u8* bounce = malloc(ata->sector_size);
    if (!bounce)
        goto done;

    size_t remaining = bytes;

    if (sector_off) {
        size_t available = ata->sector_size - sector_off;
        size_t chunk = remaining < available ? remaining : available;

        if (!ata_read_sectors(ata, (u32)lba, 1, (u16*)bounce))
            goto done_free;

        memcpy(bounce + sector_off, in, chunk);

        if (!ata_write_sectors(ata, (u32)lba, 1, (const u16*)bounce))
            goto done_free;

        in += chunk;
        remaining -= chunk;
        lba++;
    }

    while (remaining >= ata->sector_size) {
        size_t full_sectors = remaining / ata->sector_size;
        size_t batch = full_sectors;

        if (batch > ATA_MAX_PIO_SECTORS)
            batch = ATA_MAX_PIO_SECTORS;

        if (!ata_write_sectors(ata, (u32)lba, (u8)batch, (const u16*)in))
            goto done_free;

        size_t batch_bytes = batch * ata->sector_size;

        in += batch_bytes;
        remaining -= batch_bytes;
        lba += batch;
    }

    if (remaining) {
        if (!ata_read_sectors(ata, (u32)lba, 1, (u16*)bounce))
            goto done_free;

        memcpy(bounce, in, remaining);

        if (!ata_write_sectors(ata, (u32)lba, 1, (const u16*)bounce))
            goto done_free;
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

bool ata_disk_init(void) {
    ata_device_t* ata = calloc(1, sizeof(ata_device_t));
    if (!ata)
        return false;

    ata->io_base = ATA_PRIMARY_BASE;
    ata->ctrl_base = ATA_PRIMARY_CTRL;
    ata->master = true;

    sched_wait_queue_init(&ata->io_wait);
    sched_wait_queue_init(&ata->irq_wait);

    outb(ata->ctrl_base, ATA_CTRL_IRQ_ENABLE);

    u16 identify[256];
    if (!ata_identify(ata, identify)) {
        log_warn("ata: primary master not present");
        sched_wait_queue_destroy(&ata->io_wait);
        sched_wait_queue_destroy(&ata->irq_wait);
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
        sched_wait_queue_destroy(&ata->io_wait);
        sched_wait_queue_destroy(&ata->irq_wait);
        free(ata);
        return false;
    }

    ata->sector_size = ATA_SECTOR_SIZE;
    ata->sector_count = sectors;
    ata->irq_enabled = true;
    ata->irq_error = false;
    ata->irq_seq = 0;
    ata->io_busy = false;

    ata_primary = ata;
    irq_register(IRQ_PRIMARY_ATA, ata_primary_irq);

    static disk_interface_t ata_interface = {
        .read = ata_read,
        .write = ata_write,
    };

    disk_dev_t* disk = calloc(1, sizeof(disk_dev_t));
    if (!disk) {
        ata_primary = NULL;
        sched_wait_queue_destroy(&ata->io_wait);
        sched_wait_queue_destroy(&ata->irq_wait);
        free(ata);
        return false;
    }

    disk->name = strdup("hda");
    disk->type = DISK_HARD;
    disk->interface = &ata_interface;
    disk->sector_size = ata->sector_size;
    disk->sector_count = ata->sector_count;
    disk->private = ata;

    if (!disk->name || !disk_register(disk)) {
        free(disk->name);
        free(disk);
        ata_primary = NULL;
        sched_wait_queue_destroy(&ata->io_wait);
        sched_wait_queue_destroy(&ata->irq_wait);
        free(ata);
        return false;
    }

    log_info("ata: primary master ready (%zu sectors)", disk->sector_count);
    return true;
}
