#include "ata.h"

#include <arch/arch.h>
#include <base/types.h>
#include <errno.h>
#include <limits.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/pci.h>
#include <sys/time.h>
#include <x86/asm.h>
#include <x86/irq.h>

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

#define ATA_CTRL_IRQ_ENABLE  0x00
#define ATA_CTRL_IRQ_DISABLE 0x02
#define ATA_MAX_PIO_SECTORS  255
#define ATA_IRQ_TIMEOUT_MS   50
#define ATA_IRQ_MAX_WAIT_MS  250

#define ATA_PCI_BAR0 0x10
#define ATA_PCI_BAR1 0x14
#define ATA_PCI_BAR2 0x18
#define ATA_PCI_BAR3 0x1c

typedef struct {
    u16 io_base;
    u16 ctrl_base;
    bool irq_enabled;
    bool irq_force_poll;
    bool irq_error;
    u32 irq_seq;
    u32 irq_timeout_count;
    bool io_busy;
    spinlock_t io_lock;
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

typedef struct {
    ata_channel_t channels[2]; // 0 = primary, 1 = secondary
    bool irq_done[2];
    disk_dev_t *disks[4];
    bool loaded;
} ata_driver_state_t;

static ata_driver_state_t ata_driver = { 0 };

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

static bool ata_disk_size(const ata_device_t *ata, size_t *size_out) {
    if (!ata || !size_out || !ata->sector_size) {
        return false;
    }

    if (ata->sector_count > SIZE_MAX / ata->sector_size) {
        return false;
    }

    *size_out = ata->sector_count * ata->sector_size;
    return true;
}

static void ata_lock(ata_device_t *dev) {
    if (!dev || !dev->channel) {
        return;
    }

    ata_channel_t *ch = dev->channel;
    u64 wait_start = 0;
    bool warned = false;

    for (;;) {
        u32 wait_seq = sched_wait_seq(&ch->io_wait);
        unsigned long flags = spin_lock_irqsave(&ch->io_lock);

        if (!ch->io_busy) {
            ch->io_busy = true;
            spin_unlock_irqrestore(&ch->io_lock, flags);
            return;
        }

        spin_unlock_irqrestore(&ch->io_lock, flags);

        if (!wait_start) {
            wait_start = arch_timer_ticks();
        } else if (!warned) {
            u64 hz = arch_timer_hz();
            u64 elapsed = arch_timer_ticks() - wait_start;
            if (hz && elapsed > hz * 2ULL) {
                log_warn("lock wait >2s io=%#x ctrl=%#x busy=%d", ch->io_base, ch->ctrl_base, ch->io_busy ? 1 : 0);
                warned = true;
            }
        }

        if (sched_is_running() && sched_current() && ch->io_wait.list) {
            (void)sched_wait_for_change(&ch->io_wait, wait_seq);
            continue;
        }

        arch_cpu_relax();
    }
}

static void ata_unlock(ata_device_t *dev) {
    if (!dev || !dev->channel) {
        return;
    }

    ata_channel_t *ch = dev->channel;
    unsigned long flags = spin_lock_irqsave(&ch->io_lock);

    ch->io_busy = false;

    spin_unlock_irqrestore(&ch->io_lock, flags);

    if (ch->io_wait.list) {
        sched_wake_one(&ch->io_wait);
    }
}

static u32 ata_irq_seq_load(const ata_channel_t *ch) {
    if (!ch) {
        return 0;
    }

    return __atomic_load_n(&ch->irq_seq, __ATOMIC_ACQUIRE);
}

static bool ata_take_irq_error(ata_channel_t *ch) {
    if (!ch) {
        return false;
    }

    return __atomic_exchange_n(&ch->irq_error, false, __ATOMIC_ACQ_REL);
}

static u32 ata_irq_snapshot(ata_device_t *dev) {
    if (!dev || !dev->channel) {
        return 0;
    }

    ata_channel_t *ch = dev->channel;
    u32 seq = ata_irq_seq_load(ch);
    ata_take_irq_error(ch);

    return seq;
}

static sched_wait_result_t ata_wait_irq_queue(ata_channel_t *ch, u32 seq, u64 deadline) {
    return sched_wait_on(&ch->irq_wait, seq, deadline, SCHED_WAIT_INTERRUPTIBLE);
}

static bool ata_wait_irq_event(ata_device_t *dev, u32 *seq) {
    if (!dev || !dev->channel || !seq) {
        return false;
    }

    ata_channel_t *ch = dev->channel;
    if (!ch->irq_enabled || ch->irq_force_poll) {
        return true;
    }

    u64 start = arch_timer_ticks();
    u64 timeout = ms_to_ticks(ATA_IRQ_TIMEOUT_MS);
    u64 max_wait = ms_to_ticks(ATA_IRQ_MAX_WAIT_MS);

    if (!timeout) {
        timeout = 1;
    }

    if (!max_wait) {
        max_wait = timeout;
    }

    bool timeout_warned = false;

    for (;;) {
        u32 now = ata_irq_seq_load(ch);
        if (now != *seq) {
            *seq = now;
            return !ata_take_irq_error(ch);
        }

        u64 elapsed = arch_timer_ticks() - start;
        if (elapsed >= timeout) {
            u8 status = inb(ch->io_base + ATA_REG_STATUS);
            if (status & (ATA_SR_ERR | ATA_SR_DF)) {
                __atomic_store_n(&ch->irq_error, true, __ATOMIC_RELEASE);
                return false;
            }

            if (!(status & ATA_SR_BUSY)) {
                // lost or delayed IRQ: continue via status polling path
                return true;
            }

            if (!timeout_warned) {
                log_warn("irq delayed io=%#x ctrl=%#x seq=%u", ch->io_base, ch->ctrl_base, (unsigned)*seq);
                timeout_warned = true;
            }

            if (elapsed >= max_wait) {
                log_warn(
                    "irq timeout io=%#x ctrl=%#x seq=%u, switching to poll mode",
                    ch->io_base,
                    ch->ctrl_base,
                    (unsigned)*seq
                );
                ch->irq_timeout_count++;
                ch->irq_force_poll = true;
                ch->irq_enabled = false;
                outb(ch->ctrl_base, ATA_CTRL_IRQ_DISABLE);
                return true;
            }
        }

        if (sched_is_running() && sched_current() && ch->irq_wait.list) {
            sched_thread_t *current = sched_current();
            if (current && sched_signal_pending(current)) {
                return false;
            }

            u32 wait_seq = sched_wait_seq(&ch->irq_wait);
            now = ata_irq_seq_load(ch);
            if (now != *seq) {
                continue;
            }

            u64 now_ticks = arch_timer_ticks();
            u64 deadline = start + timeout;
            if ((now_ticks - start) >= timeout) {
                deadline = now_ticks + 1;
            }

            sched_wait_result_t wait_result = ata_wait_irq_queue(ch, wait_seq, deadline);

            if (wait_result == SCHED_WAIT_INTR) {
                return false;
            }

            continue;
        }

        arch_cpu_relax();
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

static bool ata_wait_drq(ata_device_t *dev, u32 *seq) {
    if (!dev || !dev->channel) {
        return false;
    }

    if (!dev->channel->irq_enabled || dev->channel->irq_force_poll) {
        return ata_poll(dev);
    }

    for (;;) {
        if (!dev->channel->irq_enabled || dev->channel->irq_force_poll) {
            return ata_poll(dev);
        }

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

static bool ata_wait_ready_event(ata_device_t *dev, u32 *seq) {
    if (!dev || !dev->channel) {
        return false;
    }

    if (!dev->channel->irq_enabled || dev->channel->irq_force_poll) {
        return ata_wait_ready(dev);
    }

    for (;;) {
        if (!dev->channel->irq_enabled || dev->channel->irq_force_poll) {
            return ata_wait_ready(dev);
        }

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
    ata_channel_t *ch = &ata_driver.channels[0];

    u8 status = inb(ch->io_base + ATA_REG_STATUS);

    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        __atomic_store_n(&ch->irq_error, true, __ATOMIC_RELEASE);
    }

    __atomic_add_fetch(&ch->irq_seq, 1, __ATOMIC_ACQ_REL);
    ch->irq_timeout_count = 0;

    if (ch->irq_wait.list) {
        sched_wake_all(&ch->irq_wait);
    }

    irq_ack(IRQ_PRIMARY_ATA);
}

static void ata_secondary_irq(UNUSED int_state_t *s) {
    ata_channel_t *ch = &ata_driver.channels[1];

    u8 status = inb(ch->io_base + ATA_REG_STATUS);

    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        __atomic_store_n(&ch->irq_error, true, __ATOMIC_RELEASE);
    }

    __atomic_add_fetch(&ch->irq_seq, 1, __ATOMIC_ACQ_REL);
    ch->irq_timeout_count = 0;

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

static bool atapi_read_capacity(ata_device_t *dev, u32 *sector_count) {
    if (!dev || !sector_count) {
        return false;
    }

    ata_select(dev, 0);

    outb(dev->channel->io_base + ATA_REG_ERROR, 0);
    outb(dev->channel->io_base + ATA_REG_LBA1, 8);
    outb(dev->channel->io_base + ATA_REG_LBA2, 0);

    u32 seq = ata_irq_snapshot(dev);

    outb(dev->channel->io_base + ATA_REG_COMMAND, ATA_CMD_PACKET);

    if (!ata_poll(dev)) {
        return false;
    }

    u8 cdb[12] = { 0 };
    cdb[0] = 0x25;

    for (size_t i = 0; i < 6; i++) {
        outw(dev->channel->io_base + ATA_REG_DATA, (u16)cdb[i * 2] | ((u16)cdb[i * 2 + 1] << 8));
    }

    if (!ata_wait_drq(dev, &seq)) {
        return false;
    }

    u8 data[8];
    for (size_t i = 0; i < 4; i++) {
        u16 word = inw(dev->channel->io_base + ATA_REG_DATA);
        data[i * 2] = (u8)(word & 0xff);
        data[i * 2 + 1] = (u8)(word >> 8);
    }

    u32 last_lba = ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | data[3];
    u32 block_len = ((u32)data[4] << 24) | ((u32)data[5] << 16) | ((u32)data[6] << 8) | data[7];

    if (block_len != ATAPI_SECTOR_SIZE || last_lba == UINT32_MAX) {
        return false;
    }

    *sector_count = last_lba + 1;
    return *sector_count != 0;
}

static bool atapi_read_sectors(ata_device_t *dev, u32 lba, u16 count, void *buffer) {
    if (!dev || !buffer || !count) {
        return false;
    }

    u8 *out = buffer;

    for (u16 s = 0; s < count; s++) {
        u32 cur_lba = lba + s;

        ata_select(dev, 0);

        outb(dev->channel->io_base + ATA_REG_ERROR, 0);
        outb(dev->channel->io_base + ATA_REG_LBA1, (u8)(ATAPI_SECTOR_SIZE & 0xff));
        outb(dev->channel->io_base + ATA_REG_LBA2, (u8)(ATAPI_SECTOR_SIZE >> 8));

        u32 seq = ata_irq_snapshot(dev);

        outb(dev->channel->io_base + ATA_REG_COMMAND, ATA_CMD_PACKET);

        // cdb phase: the device sets DRQ when ready for the command packet
        // this transition does not always assert INTRQ, so we poll for it
        if (!ata_poll(dev)) {
            return false;
        }

        // read(12) scsi cdb
        u8 cdb[12] = { 0 };
        cdb[0] = 0xa8; // read(12) opcode
        cdb[2] = (u8)((cur_lba >> 24) & 0xff);
        cdb[3] = (u8)((cur_lba >> 16) & 0xff);
        cdb[4] = (u8)((cur_lba >> 8) & 0xff);
        cdb[5] = (u8)(cur_lba & 0xff);
        cdb[9] = 1; // transfer 1 sector

        for (size_t i = 0; i < 6; i++) {
            outw(dev->channel->io_base + ATA_REG_DATA, (u16)cdb[i * 2] | ((u16)cdb[i * 2 + 1] << 8));
        }

        // data phase: the device asserts INTRQ when data is ready
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

static bool ata_read_sectors(ata_device_t *dev, u32 lba, u8 count, u16 *buffer) {
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

    u32 seq = ata_irq_snapshot(dev);

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

static bool ata_write_sectors(ata_device_t *dev, u32 lba, u8 count, const u16 *buffer) {
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

    u32 seq = ata_irq_snapshot(dev);

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

typedef struct {
    ata_device_t *ata;
    u8 *out;
    size_t lba;
    size_t off;
    size_t left;
} ata_read_cursor_t;

static size_t read_chunk(size_t left, size_t available) {
    return left < available ? left : available;
}

static bool atapi_read_bytes(ata_device_t *ata, u8 *out, size_t offset, size_t bytes) {
    u8 *bounce = malloc(ATAPI_SECTOR_SIZE);
    if (!bounce) {
        return false;
    }

    size_t left = bytes;

    while (left) {
        u32 lba = (u32)(offset / ATAPI_SECTOR_SIZE);
        size_t off = offset % ATAPI_SECTOR_SIZE;

        if (!atapi_read_sectors(ata, lba, 1, bounce)) {
            free(bounce);
            return false;
        }

        size_t chunk = read_chunk(left, ATAPI_SECTOR_SIZE - off);
        memcpy(out, bounce + off, chunk);

        out += chunk;
        left -= chunk;
        offset += chunk;
    }

    free(bounce);
    return true;
}

static bool ata_read_head(ata_read_cursor_t *read, u8 *bounce) {
    if (!read->off) {
        return true;
    }

    if (!ata_read_sectors(read->ata, (u32)read->lba, 1, (u16 *)bounce)) {
        return false;
    }

    size_t chunk = read_chunk(read->left, read->ata->sector_size - read->off);
    memcpy(read->out, bounce + read->off, chunk);

    read->out += chunk;
    read->left -= chunk;
    read->lba++;
    read->off = 0;
    return true;
}

static bool ata_read_full(ata_read_cursor_t *read) {
    while (read->left >= read->ata->sector_size) {
        size_t batch = read->left / read->ata->sector_size;
        if (batch > ATA_MAX_PIO_SECTORS) {
            batch = ATA_MAX_PIO_SECTORS;
        }

        if (!ata_read_sectors(read->ata, (u32)read->lba, (u8)batch, (u16 *)read->out)) {
            return false;
        }

        size_t batch_bytes = batch * read->ata->sector_size;
        read->out += batch_bytes;
        read->left -= batch_bytes;
        read->lba += batch;
    }

    return true;
}

static bool ata_read_tail(ata_read_cursor_t *read, u8 *bounce) {
    if (!read->left) {
        return true;
    }

    if (!ata_read_sectors(read->ata, (u32)read->lba, 1, (u16 *)bounce)) {
        return false;
    }

    memcpy(read->out, bounce, read->left);
    return true;
}

static bool ata_read_pio(ata_device_t *ata, u8 *out, size_t offset, size_t bytes) {
    u8 *bounce = malloc(ata->sector_size);
    if (!bounce) {
        return false;
    }

    ata_read_cursor_t read = {
        .ata = ata,
        .out = out,
        .lba = offset / ata->sector_size,
        .off = offset % ata->sector_size,
        .left = bytes,
    };

    bool ok = ata_read_head(&read, bounce) && ata_read_full(&read) && ata_read_tail(&read, bounce);

    free(bounce);
    return ok;
}

typedef struct {
    ata_device_t *ata;
    const u8 *in;
    size_t lba;
    size_t off;
    size_t left;
} ata_write_cursor_t;

static ssize_t ata_read(disk_dev_t *dev, void *dest, size_t offset, size_t bytes) {
    if (!dev || !dest || !dev->private) {
        return -1;
    }

    ata_device_t *ata = dev->private;
    ata_lock(ata);

    ssize_t result = -1;
    size_t disk_size = 0;

    if (!ata_disk_size(ata, &disk_size)) {
        result = -EOVERFLOW;
        goto done;
    }

    if (offset >= disk_size) {
        result = 0;
        goto done;
    }

    size_t left = disk_size - offset;
    if (bytes > left) {
        bytes = left;
    }

    if (!bytes) {
        result = 0;
        goto done;
    }

    u8 *out = dest;
    bool ok = ata->is_atapi ? atapi_read_bytes(ata, out, offset, bytes) : ata_read_pio(ata, out, offset, bytes);

    if (ok) {
        result = (ssize_t)bytes;
    }

done:
    ata_unlock(ata);
    return result;
}

static bool ata_write_head(ata_write_cursor_t *write, u8 *bounce) {
    if (!write->off) {
        return true;
    }

    size_t chunk = read_chunk(write->left, write->ata->sector_size - write->off);

    if (!ata_read_sectors(write->ata, (u32)write->lba, 1, (u16 *)bounce)) {
        return false;
    }

    memcpy(bounce + write->off, write->in, chunk);

    if (!ata_write_sectors(write->ata, (u32)write->lba, 1, (const u16 *)bounce)) {
        return false;
    }

    write->in += chunk;
    write->left -= chunk;
    write->lba++;
    write->off = 0;
    return true;
}

static bool ata_write_full(ata_write_cursor_t *write) {
    while (write->left >= write->ata->sector_size) {
        size_t batch = write->left / write->ata->sector_size;
        if (batch > ATA_MAX_PIO_SECTORS) {
            batch = ATA_MAX_PIO_SECTORS;
        }

        if (!ata_write_sectors(write->ata, (u32)write->lba, (u8)batch, (const u16 *)write->in)) {
            return false;
        }

        size_t batch_bytes = batch * write->ata->sector_size;
        write->in += batch_bytes;
        write->left -= batch_bytes;
        write->lba += batch;
    }

    return true;
}

static bool ata_write_tail(ata_write_cursor_t *write, u8 *bounce) {
    if (!write->left) {
        return true;
    }

    if (!ata_read_sectors(write->ata, (u32)write->lba, 1, (u16 *)bounce)) {
        return false;
    }

    memcpy(bounce, write->in, write->left);

    return ata_write_sectors(write->ata, (u32)write->lba, 1, (const u16 *)bounce);
}

static bool ata_write_pio(ata_device_t *ata, const u8 *in, size_t offset, size_t bytes) {
    u8 *bounce = malloc(ata->sector_size);
    if (!bounce) {
        return false;
    }

    ata_write_cursor_t write = {
        .ata = ata,
        .in = in,
        .lba = offset / ata->sector_size,
        .off = offset % ata->sector_size,
        .left = bytes,
    };

    bool ok = ata_write_head(&write, bounce) && ata_write_full(&write) && ata_write_tail(&write, bounce);

    free(bounce);
    return ok;
}

static ssize_t ata_write(disk_dev_t *dev, void *src, size_t offset, size_t bytes) {
    if (!dev || !src || !dev->private) {
        return -1;
    }

    ata_device_t *ata = dev->private;

    if (ata->is_atapi) {
        return -1;
    }

    ata_lock(ata);

    ssize_t result = -1;
    size_t disk_size = 0;

    if (!ata_disk_size(ata, &disk_size)) {
        result = -EOVERFLOW;
        goto done;
    }

    if (offset >= disk_size) {
        result = 0;
        goto done;
    }

    size_t left = disk_size - offset;
    if (bytes > left) {
        bytes = left;
    }

    if (!bytes) {
        result = 0;
        goto done;
    }

    if (ata_write_pio(ata, src, offset, bytes)) {
        result = (ssize_t)bytes;
    }

done:
    ata_unlock(ata);
    return result;
}

static bool ata_channel_present(u16 io_base) {
    u8 status = inb(io_base + ATA_REG_STATUS);
    return status != 0xff;
}

static const char *ata_disk_names[] = { "hda", "hdb", "hdc", "hdd" };
static const char *atapi_disk_names[] = { "cda", "cdb", "cdc", "cdd" };
static const char *ata_pos_names[] = { "primary master", "primary slave", "secondary master", "secondary slave" };

static disk_interface_t ata_interface = {
    .read = ata_read,
    .write = ata_write,
};

static bool ata_detect_kind(ata_device_t *ata, u16 *identify, bool *atapi_out) {
    bool found_ata = ata_identify(ata, identify);
    if (!found_ata) {
        u16 io_base = ata->channel->io_base;
        u8 lba1 = inb(io_base + ATA_REG_LBA1);
        u8 lba2 = inb(io_base + ATA_REG_LBA2);

        if (lba1 != ATAPI_LBA1_SIGNATURE || lba2 != ATAPI_LBA2_SIGNATURE) {
            return false;
        }
    }

    bool found_atapi = found_ata ? false : ata_identify_packet(ata, identify);
    if (!found_ata && !found_atapi) {
        return false;
    }

    *atapi_out = found_atapi;
    return true;
}

static bool ata_set_size(ata_device_t *ata, const u16 *identify, bool atapi) {
    if (atapi) {
        ata->sector_size = ATAPI_SECTOR_SIZE;

        u32 sectors = 0;
        if (!atapi_read_capacity(ata, &sectors)) {
            return false;
        }

        ata->sector_count = sectors;
        return true;
    }

    ata->sector_size = ATA_SECTOR_SIZE;

    u32 lba28 = (u32)identify[60] | ((u32)identify[61] << 16);
    u64 lba48 = (u64)identify[100] | ((u64)identify[101] << 16) | ((u64)identify[102] << 32) |
                ((u64)identify[103] << 48);

    size_t sectors = lba28 ? (size_t)lba28 : (size_t)lba48;
    if (sectors > 0x0fffffff) {
        sectors = 0x0fffffff;
    }

    if (!sectors) {
        return false;
    }

    ata->sector_count = sectors;
    return true;
}

static disk_dev_t *ata_make_disk(ata_device_t *ata, bool atapi, size_t dev_index) {
    disk_dev_t *disk = calloc(1, sizeof(disk_dev_t));
    if (!disk) {
        return NULL;
    }

    if (atapi) {
        size_t scale = ATAPI_SECTOR_SIZE / ATA_SECTOR_SIZE;
        if (ata->sector_count > SIZE_MAX / scale) {
            free(disk);
            return NULL;
        }

        disk->name = strdup(atapi_disk_names[dev_index]);
        disk->type = DISK_OPTICAL;
        disk->sector_size = ATA_SECTOR_SIZE;
        disk->sector_count = ata->sector_count * scale;
    } else {
        disk->name = strdup(ata_disk_names[dev_index]);
        disk->type = DISK_HARD;
        disk->sector_size = ata->sector_size;
        disk->sector_count = ata->sector_count;
    }

    disk->interface = &ata_interface;
    disk->private = ata;

    if (!disk->name) {
        free(disk->name);
        free(disk);
        return NULL;
    }

    return disk;
}

static bool ata_probe_device(ata_channel_t *ch, bool is_master, size_t dev_index) {
    ata_device_t *ata = calloc(1, sizeof(ata_device_t));
    if (!ata) {
        return false;
    }

    ata->channel = ch;
    ata->master = is_master;

    u16 identify[256];
    bool atapi = false;

    if (!ata_detect_kind(ata, identify, &atapi)) {
        free(ata);
        return false;
    }

    ata->is_atapi = atapi;

    if (!ata_set_size(ata, identify, atapi)) {
        free(ata);
        return false;
    }

    disk_dev_t *disk = ata_make_disk(ata, atapi, dev_index);
    if (!disk) {
        free(ata);
        return false;
    }

    if (!disk_register(disk)) {
        free(disk->name);
        free(disk);
        free(ata);
        return false;
    }

    if (atapi) {
        log_info("ATAPI CD-ROM on %s", ata_pos_names[dev_index]);
    } else {
        log_info("%s ready (%zu sectors)", ata_pos_names[dev_index], disk->sector_count);
    }

    if (dev_index < (sizeof(ata_driver.disks) / sizeof(ata_driver.disks[0]))) {
        ata_driver.disks[dev_index] = disk;
    }

    return true;
}

static bool ata_probe_channel(u16 io_base, u16 ctrl_base, bool is_primary, bool use_irq) {
    if (!ata_channel_present(io_base)) {
        return false;
    }

    size_t ch_index = is_primary ? 0 : 1;
    ata_channel_t *ch = &ata_driver.channels[ch_index];

    ch->io_base = io_base;
    ch->ctrl_base = ctrl_base;
    ch->irq_enabled = use_irq;
    ch->irq_force_poll = false;
    ch->irq_error = false;
    ch->irq_seq = 0;
    ch->irq_timeout_count = 0;
    ch->io_busy = false;
    spinlock_init(&ch->io_lock);

    sched_waitq_init(&ch->io_wait);
    sched_waitq_init(&ch->irq_wait);

    outb(ctrl_base, use_irq ? ATA_CTRL_IRQ_ENABLE : ATA_CTRL_IRQ_DISABLE);

    if (use_irq && !ata_driver.irq_done[ch_index]) {
        if (is_primary) {
            irq_register(IRQ_PRIMARY_ATA, ata_primary_irq);
        } else {
            irq_register(IRQ_SECONDARY_ATA, ata_secondary_irq);
        }
        ata_driver.irq_done[ch_index] = true;
    }

    // 0 = primary master, 1 = primary slave, 2 = secondary master, 3 = secondary slave
    size_t master_index = is_primary ? 0 : 2;
    size_t slave_index = master_index + 1;

    bool found_master = ata_probe_device(ch, true, master_index);
    bool found_slave = ata_probe_device(ch, false, slave_index);

    return found_master || found_slave;
}

static u16 _read_io_bar(u8 bus, u8 slot, u8 func, u16 offset) {
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
            u16 bar0 = _read_io_bar(node->bus, node->slot, node->func, ATA_PCI_BAR0);
            u16 bar1 = _read_io_bar(node->bus, node->slot, node->func, ATA_PCI_BAR1);

            if (bar0) {
                pri_io = bar0;
            }

            if (bar1) {
                pri_ctrl = (u16)(bar1 + 2);
            }
        }

        if (sec_native) {
            u16 bar2 = _read_io_bar(node->bus, node->slot, node->func, ATA_PCI_BAR2);
            u16 bar3 = _read_io_bar(node->bus, node->slot, node->func, ATA_PCI_BAR3);

            if (bar2) {
                sec_io = bar2;
            }

            if (bar3) {
                sec_ctrl = (u16)(bar3 + 2);
            }
        }

        pci_enable_bus_master(node->bus, node->slot, node->func);

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

    if (!found) {
        bool found_secondary = ata_probe_channel(ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, false, true);
        if (found_secondary) {
            found = true;
        }
    }

    if (!found) {
        log_warn("no ata devices found");
        return false;
    }

    return true;
}

bool ata_driver_busy(void) {
    for (size_t i = 0; i < (sizeof(ata_driver.disks) / sizeof(ata_driver.disks[0])); i++) {
        disk_dev_t *disk = ata_driver.disks[i];
        if (disk && disk_is_busy(disk)) {
            return true;
        }
    }

    return false;
}

driver_err_t ata_driver_load(void) {
    if (ata_driver.loaded) {
        return DRIVER_OK;
    }

    if (!ata_disk_init()) {
        return DRIVER_ERR_INIT_FAILED;
    }

    ata_driver.loaded = true;
    return DRIVER_OK;
}

driver_err_t ata_driver_unload(void) {
    if (!ata_driver.loaded) {
        return DRIVER_OK;
    }

    if (ata_driver_busy()) {
        return DRIVER_ERR_BUSY;
    }

    for (size_t i = 0; i < (sizeof(ata_driver.disks) / sizeof(ata_driver.disks[0])); i++) {
        disk_dev_t *disk = ata_driver.disks[i];
        if (!disk) {
            continue;
        }

        if (!disk_unregister(disk)) {
            return DRIVER_ERR_BUSY;
        }
    }

    for (size_t i = 0; i < (sizeof(ata_driver.disks) / sizeof(ata_driver.disks[0])); i++) {
        disk_dev_t *disk = ata_driver.disks[i];
        if (!disk) {
            continue;
        }

        ata_device_t *ata = disk->private;
        free(disk->name);
        free(disk);
        free(ata);
        ata_driver.disks[i] = NULL;
    }

    for (size_t i = 0; i < 2; i++) {
        ata_channel_t *channel = &ata_driver.channels[i];
        if (channel->ctrl_base) {
            outb(channel->ctrl_base, ATA_CTRL_IRQ_DISABLE);
        }
    }

    if (ata_driver.irq_done[0]) {
        irq_unregister(IRQ_PRIMARY_ATA);
    }

    if (ata_driver.irq_done[1]) {
        irq_unregister(IRQ_SECONDARY_ATA);
    }

    memset(ata_driver.channels, 0, sizeof(ata_driver.channels));
    memset(ata_driver.irq_done, 0, sizeof(ata_driver.irq_done));
    ata_driver.loaded = false;

    return DRIVER_OK;
}
