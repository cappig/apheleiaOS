#include "virtio.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <riscv/asm.h>
#include <stddef.h>
#include <string.h>

// minimal virtio-mmio block path for boot: read-only, single-queue
#define VIRTIO_MMIO_MAGIC_VALUE 0x000
#define VIRTIO_MMIO_VERSION 0x004
#define VIRTIO_MMIO_DEVICE_ID 0x008
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL 0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_MMIO_QUEUE_NUM 0x038
#define VIRTIO_MMIO_QUEUE_ALIGN 0x03c
#define VIRTIO_MMIO_QUEUE_PFN 0x040
#define VIRTIO_MMIO_QUEUE_READY 0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_STATUS 0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW 0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4

#define VIRTIO_MAGIC 0x74726976U
#define VIRTIO_VERSION_LEGACY 1
#define VIRTIO_VERSION_MODERN 2
#define VIRTIO_DEVICE_BLOCK 2

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER 0x02
#define VIRTIO_STATUS_DRIVER_OK 0x04
#define VIRTIO_STATUS_FEATURES_OK 0x08

#define VIRTIO_F_VERSION_1 32

#define VIRTQ_DESC_F_NEXT 0x01
#define VIRTQ_DESC_F_WRITE 0x02

#define VIRTIO_REQ_IN 0

#define VIRTIO_SECTOR_SIZE 512
#define VIRTQ_SIZE 8
#define VIRTQ_ALIGN 4096

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

struct virtio_req {
    u32 type;
    u32 reserved;
    u64 sector;
} PACKED;

struct virtio_state {
    uintptr_t base;
    u32 version;
    bool ready;
    u16 last_used;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    struct virtio_req req;
    u8 status;
    struct virtq_desc desc_modern[VIRTQ_SIZE] ALIGNED(16);
    struct virtq_avail avail_modern ALIGNED(16);
    struct virtq_used used_modern ALIGNED(16);
    u8 legacy[VIRTQ_ALIGN * 2] ALIGNED(VIRTQ_ALIGN);
};

// queue storage for modern vs legacy layout
static struct virtio_state virtio;

// common QEMU virtio-mmio slots if DTB is missing
static const uintptr_t virtio_fallback_bases[] = {
    (uintptr_t)0x10001000UL,
    (uintptr_t)0x10002000UL,
    (uintptr_t)0x10003000UL,
    (uintptr_t)0x10004000UL,
    (uintptr_t)0x10005000UL,
    (uintptr_t)0x10006000UL,
    (uintptr_t)0x10007000UL,
    (uintptr_t)0x10008000UL,
};

static inline u32 mmio_read(u32 offset) {
    return *(volatile u32 *)(virtio.base + offset);
}

static inline void mmio_write(u32 offset, u32 value) {
    *(volatile u32 *)(virtio.base + offset) = value;
}

static bool virtio_probe(uintptr_t base) {
    virtio.base = base;

    if (mmio_read(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MAGIC) {
        return false;
    }

    virtio.version = mmio_read(VIRTIO_MMIO_VERSION);
    if (virtio.version != VIRTIO_VERSION_MODERN &&
        virtio.version != VIRTIO_VERSION_LEGACY) {
        return false;
    }

    if (mmio_read(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_BLOCK) {
        return false;
    }

    return true;
}

static bool virtio_select_base(const fdt_reg_t *regs, size_t reg_count) {
    if (regs && reg_count) {
        for (size_t i = 0; i < reg_count; i++) {
            if (virtio_probe((uintptr_t)regs[i].addr)) {
                return true;
            }
        }
    }

    for (size_t i = 0; i < ARRAY_LEN(virtio_fallback_bases); i++) {
        if (virtio_probe(virtio_fallback_bases[i])) {
            return true;
        }
    }

    return false;
}

static bool virtio_negotiate_features(void) {
    u64 driver_features = 0;

    if (virtio.version != VIRTIO_VERSION_MODERN) {
        mmio_write(VIRTIO_MMIO_DRIVER_FEATURES, (u32)driver_features);
        return true;
    }

    mmio_write(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    u32 features_lo = mmio_read(VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_write(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    u32 features_hi = mmio_read(VIRTIO_MMIO_DEVICE_FEATURES);
    u64 features = ((u64)features_hi << 32) | features_lo;

    if (features & (1ull << VIRTIO_F_VERSION_1)) {
        driver_features |= (1ull << VIRTIO_F_VERSION_1);
    }

    mmio_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_write(VIRTIO_MMIO_DRIVER_FEATURES, (u32)driver_features);
    mmio_write(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    mmio_write(VIRTIO_MMIO_DRIVER_FEATURES, (u32)(driver_features >> 32));

    mmio_write(
        VIRTIO_MMIO_STATUS,
        mmio_read(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FEATURES_OK
    );

    return (mmio_read(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) != 0;
}

static void virtio_setup_queue_modern(void) {
    virtio.desc = virtio.desc_modern;
    virtio.avail = &virtio.avail_modern;
    virtio.used = &virtio.used_modern;

    u64 desc_addr = (u64)(uintptr_t)virtio.desc;
    u64 avail_addr = (u64)(uintptr_t)virtio.avail;
    u64 used_addr = (u64)(uintptr_t)virtio.used;

    mmio_write(VIRTIO_MMIO_QUEUE_DESC_LOW, (u32)desc_addr);
    mmio_write(VIRTIO_MMIO_QUEUE_DESC_HIGH, (u32)(desc_addr >> 32));
    mmio_write(VIRTIO_MMIO_QUEUE_AVAIL_LOW, (u32)avail_addr);
    mmio_write(VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (u32)(avail_addr >> 32));
    mmio_write(VIRTIO_MMIO_QUEUE_USED_LOW, (u32)used_addr);
    mmio_write(VIRTIO_MMIO_QUEUE_USED_HIGH, (u32)(used_addr >> 32));
    mmio_write(VIRTIO_MMIO_QUEUE_READY, 1);
}

static void virtio_setup_queue_legacy(void) {
    mmio_write(VIRTIO_MMIO_GUEST_PAGE_SIZE, VIRTQ_ALIGN);
    memset(virtio.legacy, 0, sizeof(virtio.legacy));

    virtio.desc = (struct virtq_desc *)virtio.legacy;
    virtio.avail =
        (struct virtq_avail *)(virtio.legacy + sizeof(struct virtq_desc) * VIRTQ_SIZE);

    uintptr_t used_addr =
        ALIGN((uintptr_t)virtio.avail + sizeof(struct virtq_avail), VIRTQ_ALIGN);
    virtio.used = (struct virtq_used *)used_addr;

    mmio_write(VIRTIO_MMIO_QUEUE_ALIGN, VIRTQ_ALIGN);
    mmio_write(VIRTIO_MMIO_QUEUE_PFN, ((u32)(uintptr_t)virtio.desc) / VIRTQ_ALIGN);
}

static bool virtio_setup_queue(void) {
    mmio_write(VIRTIO_MMIO_QUEUE_SEL, 0);
    u32 qmax = mmio_read(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (!qmax) {
        return false;
    }

    u32 qnum = (qmax < VIRTQ_SIZE) ? qmax : VIRTQ_SIZE;
    mmio_write(VIRTIO_MMIO_QUEUE_NUM, qnum);

    if (virtio.version == VIRTIO_VERSION_MODERN) {
        virtio_setup_queue_modern();
    } else {
        virtio_setup_queue_legacy();
    }

    return true;
}

bool virtio_init(const fdt_reg_t *regs, size_t reg_count) {
    if (!virtio_select_base(regs, reg_count)) {
        return false;
    }

    mmio_write(VIRTIO_MMIO_STATUS, 0);
    mmio_write(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write(
        VIRTIO_MMIO_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
    );

    if (!virtio_negotiate_features()) {
        return false;
    }

    if (!virtio_setup_queue()) {
        return false;
    }

    mmio_write(
        VIRTIO_MMIO_STATUS,
        mmio_read(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK
    );

    virtio.last_used = 0;
    virtio.ready = true;
    return true;
}

bool virtio_read(u64 sector, void *buffer, u32 sector_count) {
    if (!virtio.ready || !buffer || !sector_count) {
        return false;
    }

    // submit a single read request and spin for completion
    u32 length = sector_count * VIRTIO_SECTOR_SIZE;

    virtio.req.type = VIRTIO_REQ_IN;
    virtio.req.reserved = 0;
    virtio.req.sector = sector;
    virtio.status = 0xff;

    virtio.desc[0].addr = (u64)(uintptr_t)&virtio.req;
    virtio.desc[0].len = sizeof(virtio.req);
    virtio.desc[0].flags = VIRTQ_DESC_F_NEXT;
    virtio.desc[0].next = 1;

    virtio.desc[1].addr = (u64)(uintptr_t)buffer;
    virtio.desc[1].len = length;
    virtio.desc[1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
    virtio.desc[1].next = 2;

    virtio.desc[2].addr = (u64)(uintptr_t)&virtio.status;
    virtio.desc[2].len = 1;
    virtio.desc[2].flags = VIRTQ_DESC_F_WRITE;
    virtio.desc[2].next = 0;

    mmio_fence();
    u16 idx = virtio.avail->idx;
    virtio.avail->ring[idx % VIRTQ_SIZE] = 0;
    mmio_fence();
    virtio.avail->idx = idx + 1;
    mmio_fence();

    mmio_write(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    while (virtio.used->idx == virtio.last_used) {
        mmio_fence();
    }

    virtio.last_used++;
    mmio_fence();

    return virtio.status == 0;
}
