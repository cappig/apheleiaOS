#include <arch/arch.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <log/log.h>
#include <parse/fdt.h>
#include <riscv/asm.h>
#include <riscv/platform.h>
#include <sched/scheduler.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/lock.h>

#include "virtio_blk.h"

#define VIRTIO_MMIO_WINDOW_SIZE 0x1000U
#define VIRTIO_MMIO_MAGIC_VALUE 0x000
#define VIRTIO_MMIO_VERSION 0x004
#define VIRTIO_MMIO_DEVICE_ID 0x008
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_QUEUE_SEL 0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_MMIO_QUEUE_NUM 0x038
#define VIRTIO_MMIO_QUEUE_ALIGN 0x03c
#define VIRTIO_MMIO_QUEUE_PFN 0x040
#define VIRTIO_MMIO_QUEUE_READY 0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064
#define VIRTIO_MMIO_STATUS 0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW 0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0fc
#define VIRTIO_MMIO_DEVICE_CONFIG 0x100

#define VIRTIO_MAGIC 0x74726976U
#define VIRTIO_VERSION_LEGACY 1
#define VIRTIO_VERSION_MODERN 2
#define VIRTIO_DEVICE_BLOCK 2

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER 0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04
#define VIRTIO_STATUS_FEATURES_OK 0x08
#define VIRTIO_STATUS_FAILED 0x80

#define VIRTIO_F_VERSION_1 32

#define VIRTQ_DESC_F_NEXT 0x01
#define VIRTQ_DESC_F_WRITE 0x02

#define VIRTIO_REQ_IN  0
#define VIRTIO_REQ_OUT 1

#define VIRTIO_BLK_SECTOR_SIZE 512
#define VIRTIO_BLK_MAX_DEVS    8
#define VIRTQ_SIZE             8
#define VIRTQ_ALIGN            4096
#define VIRTIO_IO_MAX_SECTORS  128
#define VIRTIO_IO_MAX_BYTES \
    (VIRTIO_IO_MAX_SECTORS * VIRTIO_BLK_SECTOR_SIZE)

struct virtq_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} PACKED;

struct virtq_avail {
    u16 flags;
    u16 idx;
    u16 ring[VIRTQ_SIZE];
} PACKED;

struct virtq_used_elem {
    u32 id;
    u32 len;
} PACKED;

struct virtq_used {
    u16 flags;
    u16 idx;
    struct virtq_used_elem ring[VIRTQ_SIZE];
} PACKED;

struct virtio_blk_req {
    u32 type;
    u32 reserved;
    u64 sector;
} PACKED;

typedef struct {
    u64 base_paddr;
    volatile u8 *mmio;
    u32 version;
    u32 irq;
    u64 sector_count;
    bool ready;
    bool registered;
    u16 last_used;
    mutex_t io_lock;
    sched_wait_queue_t io_wait;
    bool io_wait_ready;
    disk_dev_t *disk;
    struct virtio_blk_req req;
    u8 status;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    struct virtq_desc desc_modern[VIRTQ_SIZE] ALIGNED(16);
    struct virtq_avail avail_modern ALIGNED(16);
    struct virtq_used used_modern ALIGNED(16);
    u8 io_bounce[VIRTIO_IO_MAX_BYTES] ALIGNED(16);
    u8 legacy[VIRTQ_ALIGN * 2] ALIGNED(VIRTQ_ALIGN);
} riscv_virtio_blk_dev_t;

static const uintptr_t virtio_fallback_bases[] = {
    0x10001000UL,
    0x10002000UL,
    0x10003000UL,
    0x10004000UL,
    0x10005000UL,
    0x10006000UL,
    0x10007000UL,
    0x10008000UL,
};

static riscv_virtio_blk_dev_t virtio_devs[VIRTIO_BLK_MAX_DEVS];
static size_t virtio_dev_count = 0;
static bool virtio_driver_loaded = false;

const driver_desc_t riscv_virtio_blk_driver_desc = {
    .name = "riscv-virtio-blk",
    .deps = NULL,
    .stage = DRIVER_STAGE_STORAGE,
    .load = riscv_virtio_blk_driver_load,
    .unload = riscv_virtio_blk_driver_unload,
    .is_busy = riscv_virtio_blk_driver_busy,
};

static inline u32 _mmio_read32(riscv_virtio_blk_dev_t *dev, u32 offset) {
    return *(volatile u32 *)(dev->mmio + offset);
}

static inline void
_mmio_write32(riscv_virtio_blk_dev_t *dev, u32 offset, u32 value) {
    *(volatile u32 *)(dev->mmio + offset) = value;
}

static u64 _mmio_config_read64(riscv_virtio_blk_dev_t *dev, u32 offset) {
    if (!dev) {
        return 0;
    }

    if (dev->version == VIRTIO_VERSION_MODERN) {
        for (;;) {
            u32 gen_before = _mmio_read32(dev, VIRTIO_MMIO_CONFIG_GENERATION);
            u32 lo = _mmio_read32(dev, offset);
            u32 hi = _mmio_read32(dev, offset + 4);
            u32 gen_after = _mmio_read32(dev, VIRTIO_MMIO_CONFIG_GENERATION);
            if (gen_before == gen_after) {
                return ((u64)hi << 32) | lo;
            }
        }
    }

    u32 lo = _mmio_read32(dev, offset);
    u32 hi = _mmio_read32(dev, offset + 4);
    return ((u64)hi << 32) | lo;
}

static bool _virtio_negotiate_features(riscv_virtio_blk_dev_t *dev) {
    u64 driver_features = 0;

    if (dev->version != VIRTIO_VERSION_MODERN) {
        _mmio_write32(dev, VIRTIO_MMIO_DRIVER_FEATURES, (u32)driver_features);
        return true;
    }

    _mmio_write32(dev, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    u32 features_lo = _mmio_read32(dev, VIRTIO_MMIO_DEVICE_FEATURES);
    _mmio_write32(dev, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    u32 features_hi = _mmio_read32(dev, VIRTIO_MMIO_DEVICE_FEATURES);
    u64 features = ((u64)features_hi << 32) | features_lo;

    if (features & (1ULL << VIRTIO_F_VERSION_1)) {
        driver_features |= (1ULL << VIRTIO_F_VERSION_1);
    }

    _mmio_write32(dev, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    _mmio_write32(dev, VIRTIO_MMIO_DRIVER_FEATURES, (u32)driver_features);
    _mmio_write32(dev, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    _mmio_write32(dev, VIRTIO_MMIO_DRIVER_FEATURES, (u32)(driver_features >> 32));

    _mmio_write32(
        dev,
        VIRTIO_MMIO_STATUS,
        _mmio_read32(dev, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FEATURES_OK
    );

    return (_mmio_read32(dev, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) != 0;
}

static void _virtio_setup_queue_modern(riscv_virtio_blk_dev_t *dev) {
    dev->desc = dev->desc_modern;
    dev->avail = &dev->avail_modern;
    dev->used = &dev->used_modern;

    u64 desc_addr = (u64)(uintptr_t)dev->desc;
    u64 avail_addr = (u64)(uintptr_t)dev->avail;
    u64 used_addr = (u64)(uintptr_t)dev->used;

    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_DESC_LOW, (u32)desc_addr);
    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_DESC_HIGH, (u32)(desc_addr >> 32));
    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (u32)avail_addr);
    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (u32)(avail_addr >> 32));
    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_USED_LOW, (u32)used_addr);
    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_USED_HIGH, (u32)(used_addr >> 32));
    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_READY, 1);
}

static void _virtio_setup_queue_legacy(riscv_virtio_blk_dev_t *dev) {
    _mmio_write32(dev, VIRTIO_MMIO_GUEST_PAGE_SIZE, VIRTQ_ALIGN);
    memset(dev->legacy, 0, sizeof(dev->legacy));

    dev->desc = (struct virtq_desc *)dev->legacy;
    dev->avail =
        (struct virtq_avail *)(dev->legacy + sizeof(struct virtq_desc) * VIRTQ_SIZE);

    uintptr_t used_addr =
        ALIGN((uintptr_t)dev->avail + sizeof(struct virtq_avail), VIRTQ_ALIGN);
    dev->used = (struct virtq_used *)used_addr;

    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_ALIGN, VIRTQ_ALIGN);
    _mmio_write32(
        dev,
        VIRTIO_MMIO_QUEUE_PFN,
        ((u32)(uintptr_t)dev->desc) / VIRTQ_ALIGN
    );
}

static bool _virtio_setup_queue(riscv_virtio_blk_dev_t *dev) {
    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_SEL, 0);
    u32 qmax = _mmio_read32(dev, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (!qmax) {
        return false;
    }

    u32 qnum = (qmax < VIRTQ_SIZE) ? qmax : VIRTQ_SIZE;
    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_NUM, qnum);

    if (dev->version == VIRTIO_VERSION_MODERN) {
        _virtio_setup_queue_modern(dev);
    } else {
        _virtio_setup_queue_legacy(dev);
    }

    return true;
}

static bool _virtio_probe(riscv_virtio_blk_dev_t *dev, u64 base_paddr) {
    if (!dev || !base_paddr) {
        return false;
    }

    dev->mmio = arch_phys_map(base_paddr, VIRTIO_MMIO_WINDOW_SIZE, PHYS_MAP_MMIO);
    if (!dev->mmio) {
        return false;
    }

    if (_mmio_read32(dev, VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC) {
        return false;
    }

    dev->version = _mmio_read32(dev, VIRTIO_MMIO_VERSION);
    if (dev->version != VIRTIO_VERSION_MODERN &&
        dev->version != VIRTIO_VERSION_LEGACY) {
        return false;
    }

    if (_mmio_read32(dev, VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_BLOCK) {
        return false;
    }

    dev->base_paddr = base_paddr;
    mutex_init(&dev->io_lock);
    sched_wait_queue_init(&dev->io_wait);
    dev->io_wait_ready = true;

    _mmio_write32(dev, VIRTIO_MMIO_STATUS, 0);
    _mmio_write32(dev, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    _mmio_write32(
        dev,
        VIRTIO_MMIO_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
    );

    if (!_virtio_negotiate_features(dev) || !_virtio_setup_queue(dev)) {
        _mmio_write32(dev, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    _mmio_write32(
        dev,
        VIRTIO_MMIO_STATUS,
        _mmio_read32(dev, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK
    );

    dev->sector_count = _mmio_config_read64(dev, VIRTIO_MMIO_DEVICE_CONFIG);
    dev->last_used = 0;
    dev->ready = dev->sector_count != 0;
    return dev->ready;
}

static void _virtio_irq_handler(u32 irq, void *ctx) {
    riscv_virtio_blk_dev_t *dev = ctx;
    if (!dev || !dev->ready || dev->irq != irq) {
        return;
    }

    u32 status = _mmio_read32(dev, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!status) {
        return;
    }

    _mmio_write32(dev, VIRTIO_MMIO_INTERRUPT_ACK, status);
    mmio_fence();

    if (dev->io_wait_ready && sched_is_running()) {
        sched_wake_all(&dev->io_wait);
    }
}

static bool _virtio_wait_for_completion(riscv_virtio_blk_dev_t *dev) {
    if (!dev) {
        return false;
    }

    bool can_block =
        dev->irq && dev->io_wait_ready && sched_is_running() && sched_current() &&
        arch_irq_enabled() && !sched_preempt_disabled() &&
        !lock_spin_held_on_cpu();

    while (dev->used->idx == dev->last_used) {
        mmio_fence();

        if (dev->used->idx != dev->last_used) {
            break;
        }

        if (!can_block) {
            if (sched_is_running() && sched_current()) {
                sched_yield();
            } else {
                arch_cpu_relax();
            }
            continue;
        }

        u32 wait_seq = sched_wait_seq(&dev->io_wait);
        if (dev->used->idx != dev->last_used) {
            break;
        }

        u32 hz = arch_timer_hz();
        u64 timeout_ticks = hz / 200U;
        if (!timeout_ticks) {
            timeout_ticks = 1;
        }

        u64 deadline = arch_timer_ticks() + timeout_ticks;
        (void)sched_wait_on_queue(&dev->io_wait, wait_seq, deadline, 0);
    }

    return true;
}

static bool _virtio_rw_sectors_locked(
    riscv_virtio_blk_dev_t *dev,
    u64 sector,
    void *buffer,
    u32 count,
    bool write
) {
    if (!dev || !dev->ready || !buffer || !count) {
        return false;
    }

    dev->req.type = write ? VIRTIO_REQ_OUT : VIRTIO_REQ_IN;
    dev->req.reserved = 0;
    dev->req.sector = sector;
    dev->status = 0xff;

    u32 length = count * VIRTIO_BLK_SECTOR_SIZE;

    dev->desc[0].addr = (u64)(uintptr_t)&dev->req;
    dev->desc[0].len = sizeof(dev->req);
    dev->desc[0].flags = VIRTQ_DESC_F_NEXT;
    dev->desc[0].next = 1;

    dev->desc[1].addr = (u64)(uintptr_t)buffer;
    dev->desc[1].len = length;
    dev->desc[1].flags = VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE);
    dev->desc[1].next = 2;

    dev->desc[2].addr = (u64)(uintptr_t)&dev->status;
    dev->desc[2].len = 1;
    dev->desc[2].flags = VIRTQ_DESC_F_WRITE;
    dev->desc[2].next = 0;

    mmio_fence();
    u16 idx = dev->avail->idx;
    dev->avail->ring[idx % VIRTQ_SIZE] = 0;
    mmio_fence();
    dev->avail->idx = idx + 1;
    mmio_fence();

    _mmio_write32(dev, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    if (!_virtio_wait_for_completion(dev)) {
        return false;
    }

    dev->last_used++;
    mmio_fence();

    bool ok = dev->status == 0;
    return ok;
}

static bool _virtio_rw_sectors(
    riscv_virtio_blk_dev_t *dev,
    u64 sector,
    void *buffer,
    u32 count,
    bool write
) {
    if (!dev || !dev->ready || !buffer || !count) {
        return false;
    }

    mutex_lock(&dev->io_lock);
    bool ok = _virtio_rw_sectors_locked(dev, sector, buffer, count, write);
    mutex_unlock(&dev->io_lock);
    return ok;
}

static ssize_t _disk_transfer(
    riscv_virtio_blk_dev_t *dev,
    void *buf,
    size_t offset,
    size_t bytes,
    bool write
) {
    if (!dev || !buf) {
        return -1;
    }

    size_t total_size = (size_t)dev->sector_count * VIRTIO_BLK_SECTOR_SIZE;
    if (offset > total_size || bytes > total_size - offset) {
        return -1;
    }

    u8 *data = buf;
    size_t done = 0;

    while (done < bytes) {
        size_t cur_offset = offset + done;
        u64 sector = cur_offset / VIRTIO_BLK_SECTOR_SIZE;
        size_t sector_offset = cur_offset % VIRTIO_BLK_SECTOR_SIZE;
        size_t remaining = bytes - done;

        if (!sector_offset && remaining >= VIRTIO_BLK_SECTOR_SIZE) {
            size_t max_bytes = VIRTIO_IO_MAX_BYTES;
            size_t chunk_bytes = remaining < max_bytes ? remaining : max_bytes;
            u32 sector_count = (u32)(chunk_bytes / VIRTIO_BLK_SECTOR_SIZE);

            mutex_lock(&dev->io_lock);

            if (write) {
                memcpy(dev->io_bounce, data + done, chunk_bytes);
            }

            bool ok = _virtio_rw_sectors_locked(
                dev,
                sector,
                dev->io_bounce,
                sector_count,
                write
            );

            if (ok && !write) {
                memcpy(data + done, dev->io_bounce, chunk_bytes);
            }

            mutex_unlock(&dev->io_lock);

            if (!ok) {
                return -1;
            }

            done += chunk_bytes;
            continue;
        }

        u8 scratch[VIRTIO_BLK_SECTOR_SIZE];
        if (!_virtio_rw_sectors(dev, sector, scratch, 1, false)) {
            return -1;
        }

        size_t chunk = VIRTIO_BLK_SECTOR_SIZE - sector_offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        if (write) {
            memcpy(scratch + sector_offset, data + done, chunk);
            if (!_virtio_rw_sectors(dev, sector, scratch, 1, true)) {
                return -1;
            }
        } else {
            memcpy(data + done, scratch + sector_offset, chunk);
        }

        done += chunk;
    }

    return (ssize_t)done;
}

static ssize_t
_disk_read(disk_dev_t *disk, void *dest, size_t offset, size_t bytes) {
    return _disk_transfer(disk ? disk->private : NULL, dest, offset, bytes, false);
}

static ssize_t
_disk_write(disk_dev_t *disk, void *src, size_t offset, size_t bytes) {
    return _disk_transfer(disk ? disk->private : NULL, src, offset, bytes, true);
}

static bool _register_disk(riscv_virtio_blk_dev_t *dev) {
    if (!dev || !dev->sector_count) {
        return false;
    }

    static disk_interface_t disk_if = {
        .read = _disk_read,
        .write = _disk_write,
    };

    disk_dev_t *disk = calloc(1, sizeof(*disk));
    if (!disk) {
        return false;
    }

    disk->type = DISK_HARD;
    disk->sector_size = VIRTIO_BLK_SECTOR_SIZE;
    disk->sector_count = (size_t)dev->sector_count;
    disk->interface = &disk_if;
    disk->private = dev;

    if (!disk_register(disk)) {
        free(disk);
        return false;
    }

    dev->disk = disk;
    dev->registered = true;
    return true;
}

static u32 _virtio_fallback_irq(u64 base_paddr) {
    for (size_t i = 0; i < ARRAY_LEN(virtio_fallback_bases); i++) {
        if ((u64)virtio_fallback_bases[i] == base_paddr) {
            return (u32)(i + 1);
        }
    }

    return 0;
}

static bool _probe_candidate(u64 base_paddr, u32 irq) {
    if (!base_paddr || virtio_dev_count >= VIRTIO_BLK_MAX_DEVS) {
        return false;
    }

    for (size_t i = 0; i < virtio_dev_count; i++) {
        if (virtio_devs[i].base_paddr == base_paddr) {
            return false;
        }
    }

    riscv_virtio_blk_dev_t *dev = &virtio_devs[virtio_dev_count];
    memset(dev, 0, sizeof(*dev));

    if (!_virtio_probe(dev, base_paddr) || !_register_disk(dev)) {
        memset(dev, 0, sizeof(*dev));
        return false;
    }

    dev->irq = irq;
    if (dev->irq) {
        (void)riscv_irq_register(dev->irq, _virtio_irq_handler, dev);
    }

    virtio_dev_count++;
    log_info(
        "registered virtio-blk disk at %#llx (%llu sectors)",
        (unsigned long long)base_paddr,
        (unsigned long long)dev->sector_count
    );
    return true;
}

driver_err_t riscv_virtio_blk_driver_load(void) {
    if (virtio_driver_loaded) {
        return DRIVER_ERR_ALREADY_LOADED;
    }

    virtio_dev_count = 0;
    memset(virtio_devs, 0, sizeof(virtio_devs));

    const void *dtb = riscv_boot_dtb();
    fdt_reg_t regs[VIRTIO_BLK_MAX_DEVS];
    u32 irqs[VIRTIO_BLK_MAX_DEVS];
    size_t count = 0;
    size_t irq_count = 0;

    if (dtb && fdt_find_compatible_regs(
            dtb,
            "virtio,mmio",
            regs,
            sizeof(regs) / sizeof(regs[0]),
            &count
        )) {
        (void)fdt_find_compatible_irqs(
            dtb,
            "virtio,mmio",
            irqs,
            sizeof(irqs) / sizeof(irqs[0]),
            &irq_count
        );

        for (size_t i = 0; i < count; i++) {
            u32 irq = (i < irq_count) ? irqs[i] : 0;
            (void)_probe_candidate(regs[i].addr, irq);
        }
    }

    for (size_t i = 0; i < ARRAY_LEN(virtio_fallback_bases); i++) {
        (void)_probe_candidate(
            virtio_fallback_bases[i],
            _virtio_fallback_irq((u64)virtio_fallback_bases[i])
        );
    }

    virtio_driver_loaded = true;

    if (!virtio_dev_count) {
        log_info("no virtio-mmio block devices detected");
    }

    return DRIVER_OK;
}

driver_err_t riscv_virtio_blk_driver_unload(void) {
    if (!virtio_driver_loaded) {
        return DRIVER_ERR_NOT_LOADED;
    }

    for (size_t i = 0; i < virtio_dev_count; i++) {
        riscv_virtio_blk_dev_t *dev = &virtio_devs[i];
        if (dev->irq) {
            riscv_irq_unregister(dev->irq);
            dev->irq = 0;
        }
        if (dev->io_wait_ready) {
            sched_wait_queue_destroy(&dev->io_wait);
            dev->io_wait_ready = false;
        }
        if (dev->registered && dev->disk) {
            if (!disk_unregister(dev->disk)) {
                return DRIVER_ERR_BUSY;
            }
            free(dev->disk->name);
            free(dev->disk);
            dev->disk = NULL;
            dev->registered = false;
        }
    }

    virtio_dev_count = 0;
    memset(virtio_devs, 0, sizeof(virtio_devs));
    virtio_driver_loaded = false;
    return DRIVER_OK;
}

bool riscv_virtio_blk_driver_busy(void) {
    for (size_t i = 0; i < virtio_dev_count; i++) {
        if (virtio_devs[i].disk && disk_is_busy(virtio_devs[i].disk)) {
            return true;
        }
    }

    return false;
}
