#include "usb_msc.h"

#include <base/macros.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/disk.h>
#include <sys/usb.h>

#define USB_MSC_MAX_LUNS        32
#define USB_MSC_CMD_TIMEOUT_MS  1000
#define USB_MSC_IO_TIMEOUT_MS   3000

#define USB_REQ_TYPE_CLASS      0x20
#define USB_REQ_RECIP_INTERFACE 0x01

#define USB_MSC_GET_MAX_LUN     0xfe
#define USB_MSC_CBI_ADSC        0x00

#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_CAPACITY_16   0x9e
#define SCSI_SERVICE_ACTION_RC16 0x10
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2a

#define MSC_CBW_SIGNATURE 0x43425355U
#define MSC_CSW_SIGNATURE 0x53425355U

typedef struct PACKED {
    u32 signature;
    u32 tag;
    u32 transfer_length;
    u8 flags;
    u8 lun;
    u8 cdb_length;
    u8 cdb[16];
} msc_cbw_t;

typedef struct PACKED {
    u32 signature;
    u32 tag;
    u32 residue;
    u8 status;
} msc_csw_t;

typedef struct {
    bool used;
    bool online;
    usb_device_handle_t device;
    size_t hcd_id;
    size_t port;
    u8 lun;
    u8 subclass;
    u8 protocol;
    u8 transport;
    u8 interface_number;
    u8 bulk_in_ep;
    u8 bulk_out_ep;
    size_t block_size;
    size_t block_count;
    mutex_t io_lock;
    disk_dev_t *disk;
} usb_msc_lun_t;

static bool msc_registered = false;
static usb_msc_lun_t msc_luns[USB_MSC_MAX_LUNS] = {0};
static spinlock_t msc_state_lock = SPINLOCK_INIT;
static u32 next_tag = 1;

const driver_desc_t usb_msc_driver_desc = {
    .name = "usb-msc",
    .deps = NULL,
    .stage = DRIVER_STAGE_STORAGE,
    .load = usb_msc_driver_load,
    .unload = usb_msc_driver_unload,
    .is_busy = usb_msc_driver_busy,
};

typedef enum {
    MSC_TRANSPORT_BOT = 0,
    MSC_TRANSPORT_CBI,
} msc_transport_t;

typedef enum {
    MSC_BOT_XFER_OK = 0,
    MSC_BOT_XFER_COMMAND_FAIL,
    MSC_BOT_XFER_TRANSPORT_FAIL,
} msc_bot_xfer_result_t;


static u32 _be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static u64 _be64(const u8 *p) {
    return ((u64)_be32(p) << 32) | (u64)_be32(p + 4);
}

static u32 _next_tag(void) {
    unsigned long flags = arch_irq_save();
    u32 tag = next_tag++;
    if (!next_tag) {
        next_tag = 1;
    }
    arch_irq_restore(flags);
    return tag;
}

static bool _usb_msc_match(const usb_device_identity_t *identity) {
    if (!identity) {
        return false;
    }

    if (identity->interface_class == USB_CLASS_MASS_STORAGE) {
        return true;
    }

    return identity->device_class == USB_CLASS_MASS_STORAGE;
}

static bool _msc_subclass_supported(u8 subclass) {
    switch (subclass) {
    case USB_MSC_SUBCLASS_RBC:
    case USB_MSC_SUBCLASS_ATAPI:
    case USB_MSC_SUBCLASS_QIC_157:
    case USB_MSC_SUBCLASS_UFI:
    case USB_MSC_SUBCLASS_SFF8070I:
    case USB_MSC_SUBCLASS_SCSI:
        return true;
    default:
        return false;
    }
}

static bool _msc_protocol_supported(u8 protocol) {
    switch (protocol) {
    case USB_MSC_PROTO_CBI:
    case USB_MSC_PROTO_CBI_NOINTR:
    case USB_MSC_PROTO_BOT:
    case USB_MSC_PROTO_UAS:
        return true;
    default:
        return false;
    }
}

static msc_transport_t _msc_transport_for_protocol(u8 protocol) {
    switch (protocol) {
    case USB_MSC_PROTO_CBI:
    case USB_MSC_PROTO_CBI_NOINTR:
        return MSC_TRANSPORT_CBI;
    case USB_MSC_PROTO_UAS:
    case USB_MSC_PROTO_BOT:
    default:
        return MSC_TRANSPORT_BOT;
    }
}

static msc_bot_xfer_result_t _msc_bot_transfer_once(
    usb_msc_lun_t *lun,
    const void *cdb,
    size_t cdb_len,
    void *data,
    size_t data_len,
    bool data_in,
    u32 timeout_ms
) {
    if (!lun || !lun->online || !cdb || !cdb_len || cdb_len > 16) {
        return MSC_BOT_XFER_TRANSPORT_FAIL;
    }

    if (data_len && !data) {
        return MSC_BOT_XFER_TRANSPORT_FAIL;
    }

    msc_cbw_t cbw = {0};
    cbw.signature = MSC_CBW_SIGNATURE;
    cbw.tag = _next_tag();
    cbw.transfer_length = (u32)data_len;
    cbw.flags = data_in ? USB_ENDPOINT_DIR_IN : USB_ENDPOINT_DIR_OUT;
    cbw.lun = lun->lun;
    cbw.cdb_length = (u8)cdb_len;
    memcpy(cbw.cdb, cdb, cdb_len);

    size_t actual = 0;

    if (
        !usb_device_bulk_transfer(
            lun->device,
            lun->bulk_out_ep,
            &cbw,
            sizeof(cbw),
            timeout_ms,
            &actual
        ) ||
        actual != sizeof(cbw)
    ) {
        return MSC_BOT_XFER_TRANSPORT_FAIL;
    }

    bool data_ok = true;
    size_t data_actual = 0;
    if (data_len) {
        u8 ep = data_in ? lun->bulk_in_ep : lun->bulk_out_ep;

        if (
            !usb_device_bulk_transfer(
                lun->device,
                ep,
                data,
                data_len,
                timeout_ms,
                &actual
            )
        ) {
            data_ok = false;
        }

        data_actual = actual;
    }

    msc_csw_t csw = {0};

    if (
        !usb_device_bulk_transfer(
            lun->device,
            lun->bulk_in_ep,
            &csw,
            sizeof(csw),
            timeout_ms,
            &actual
        ) ||
        actual != sizeof(csw)
    ) {
        return MSC_BOT_XFER_TRANSPORT_FAIL;
    }

    if (csw.signature != MSC_CSW_SIGNATURE || csw.tag != cbw.tag) {
        return MSC_BOT_XFER_TRANSPORT_FAIL;
    }

    if (csw.status != 0) {
        return MSC_BOT_XFER_COMMAND_FAIL;
    }

    if (
        data_len &&
        (!data_ok || data_actual != data_len || csw.residue != 0)
    ) {
        return MSC_BOT_XFER_TRANSPORT_FAIL;
    }

    return MSC_BOT_XFER_OK;
}

static bool _msc_bot_transfer(
    usb_msc_lun_t *lun,
    const void *cdb,
    size_t cdb_len,
    void *data,
    size_t data_len,
    bool data_in,
    u32 timeout_ms
) {
    msc_bot_xfer_result_t result = _msc_bot_transfer_once(
        lun,
        cdb,
        cdb_len,
        data,
        data_len,
        data_in,
        timeout_ms
    );

    if (result == MSC_BOT_XFER_OK) {
        return true;
    }

    if (result == MSC_BOT_XFER_COMMAND_FAIL) {
        return false;
    }

    return false;
}

static bool _msc_cbi_transfer_once(
    usb_msc_lun_t *lun,
    const void *cdb,
    size_t cdb_len,
    void *data,
    size_t data_len,
    bool data_in,
    u32 timeout_ms
) {
    if (!lun || !lun->online || !cdb || !cdb_len || cdb_len > 12) {
        return false;
    }

    if (data_len && !data) {
        return false;
    }

    u8 cmd_block[12] = {0};
    memcpy(cmd_block, cdb, cdb_len);

    usb_setup_packet_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE,
        .request = USB_MSC_CBI_ADSC,
        .value = 0,
        .index = lun->interface_number,
        .length = sizeof(cmd_block),
    };

    size_t actual = 0;
    if (
        !usb_device_control_transfer(
            lun->device,
            &setup,
            cmd_block,
            sizeof(cmd_block),
            timeout_ms,
            &actual
        ) ||
        actual != sizeof(cmd_block)
    ) {
        return false;
    }

    if (!data_len) {
        return true;
    }

    u8 ep = data_in ? lun->bulk_in_ep : lun->bulk_out_ep;
    return usb_device_bulk_transfer(
        lun->device,
        ep,
        data,
        data_len,
        timeout_ms,
        &actual
    ) && actual == data_len;
}

static bool _msc_cbi_transfer(
    usb_msc_lun_t *lun,
    const void *cdb,
    size_t cdb_len,
    void *data,
    size_t data_len,
    bool data_in,
    u32 timeout_ms
) {
    return _msc_cbi_transfer_once(
        lun,
        cdb,
        cdb_len,
        data,
        data_len,
        data_in,
        timeout_ms
    );
}

static bool _msc_transfer(
    usb_msc_lun_t *lun,
    const void *cdb,
    size_t cdb_len,
    void *data,
    size_t data_len,
    bool data_in,
    u32 timeout_ms
) {
    if (!lun) {
        return false;
    }

    switch ((msc_transport_t)lun->transport) {
    case MSC_TRANSPORT_CBI:
        return _msc_cbi_transfer(
            lun,
            cdb,
            cdb_len,
            data,
            data_len,
            data_in,
            timeout_ms
        );
    case MSC_TRANSPORT_BOT:
    default:
        return _msc_bot_transfer(
            lun,
            cdb,
            cdb_len,
            data,
            data_len,
            data_in,
            timeout_ms
        );
    }
}

static bool _msc_test_unit_ready(usb_msc_lun_t *lun) {
    u8 cdb[6] = {0};
    cdb[0] = SCSI_TEST_UNIT_READY;
    return _msc_transfer(lun, cdb, sizeof(cdb), NULL, 0, true, USB_MSC_CMD_TIMEOUT_MS);
}

static bool _msc_inquiry(usb_msc_lun_t *lun) {
    u8 cdb[6] = {0};
    u8 resp[36] = {0};

    cdb[0] = SCSI_INQUIRY;
    cdb[4] = sizeof(resp);

    return _msc_transfer(
        lun,
        cdb,
        sizeof(cdb),
        resp,
        sizeof(resp),
        true,
        USB_MSC_CMD_TIMEOUT_MS
    );
}

static bool _msc_read_capacity16(
    usb_msc_lun_t *lun,
    size_t *out_blocks,
    size_t *out_block_size
) {
    if (!lun || !out_blocks || !out_block_size) {
        return false;
    }

    u8 cdb[16] = {0};
    u8 resp[32] = {0};

    cdb[0] = SCSI_READ_CAPACITY_16;
    cdb[1] = SCSI_SERVICE_ACTION_RC16;
    cdb[13] = sizeof(resp);

    if (
        !_msc_transfer(
            lun,
            cdb,
            sizeof(cdb),
            resp,
            sizeof(resp),
            true,
            USB_MSC_CMD_TIMEOUT_MS
        )
    ) {
        return false;
    }

    u64 last_lba = _be64(resp + 0);
    u32 block_size = _be32(resp + 8);

    if (!block_size || last_lba == 0xffffffffffffffffULL) {
        return false;
    }

    if (last_lba + 1ULL > (u64)(size_t)-1) {
        return false;
    }

    *out_blocks = (size_t)(last_lba + 1ULL);
    *out_block_size = (size_t)block_size;
    return true;
}

static bool _msc_read_capacity10(
    usb_msc_lun_t *lun,
    size_t *out_blocks,
    size_t *out_block_size
) {
    if (!lun || !out_blocks || !out_block_size) {
        return false;
    }

    u8 cdb[10] = {0};
    u8 resp[8] = {0};

    cdb[0] = SCSI_READ_CAPACITY_10;

    if (
        !_msc_transfer(
            lun,
            cdb,
            sizeof(cdb),
            resp,
            sizeof(resp),
            true,
            USB_MSC_CMD_TIMEOUT_MS
        )
    ) {
        return false;
    }

    u32 last_lba = _be32(resp + 0);
    u32 block_size = _be32(resp + 4);

    if (!block_size) {
        return false;
    }

    if (last_lba == 0xffffffffU) {
        return _msc_read_capacity16(lun, out_blocks, out_block_size);
    }

    if ((u64)last_lba + 1ULL > (u64)(size_t)-1) {
        return false;
    }

    *out_blocks = (size_t)last_lba + 1;
    *out_block_size = (size_t)block_size;
    return true;
}

static bool _msc_rw10(
    usb_msc_lun_t *lun,
    u32 lba,
    u16 blocks,
    void *buffer,
    bool write
) {
    if (!lun || !blocks || !buffer || !lun->block_size) {
        return false;
    }

    if ((size_t)blocks > ((size_t)-1 / lun->block_size)) {
        return false;
    }

    u8 cdb[10] = {0};
    cdb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
    cdb[2] = (u8)((lba >> 24) & 0xffU);
    cdb[3] = (u8)((lba >> 16) & 0xffU);
    cdb[4] = (u8)((lba >> 8) & 0xffU);
    cdb[5] = (u8)(lba & 0xffU);
    cdb[7] = (u8)((blocks >> 8) & 0xffU);
    cdb[8] = (u8)(blocks & 0xffU);

    size_t xfer_len = (size_t)blocks * lun->block_size;
    return _msc_transfer(
        lun,
        cdb,
        sizeof(cdb),
        buffer,
        xfer_len,
        !write,
        USB_MSC_IO_TIMEOUT_MS
    );
}

static ssize_t _usb_msc_read(disk_dev_t *disk, void *dest, size_t offset, size_t bytes) {
    if (!disk || !dest || !disk->private) {
        return -1;
    }

    usb_msc_lun_t *lun = disk->private;

    if (!lun->online || !lun->block_size || !lun->block_count) {
        return -1;
    }

    if (lun->block_count > ((size_t)-1 / lun->block_size)) {
        return -1;
    }

    size_t disk_size = lun->block_count * lun->block_size;

    if (offset >= disk_size) {
        return 0;
    }

    if (bytes > disk_size - offset) {
        bytes = disk_size - offset;
    }

    if (!bytes) {
        return 0;
    }

    mutex_lock(&lun->io_lock);

    ssize_t ret = -1;
    size_t block_size = lun->block_size;
    u8 *out = dest;
    size_t remaining = bytes;
    u64 lba64 = offset / block_size;
    size_t block_off = offset % block_size;

    u8 *bounce = NULL;
    if (block_off || (remaining && remaining < block_size)) {
        bounce = malloc(block_size);
        if (!bounce) {
            goto done;
        }
    }

    if (lba64 > 0xffffffffULL) {
        goto done;
    }

    if (block_off) {
        if (!_msc_rw10(lun, (u32)lba64, 1, bounce, false)) {
            goto done;
        }

        size_t chunk = block_size - block_off;
        if (chunk > remaining) {
            chunk = remaining;
        }

        memcpy(out, bounce + block_off, chunk);
        out += chunk;
        remaining -= chunk;
        lba64++;
    }

    while (remaining >= block_size) {
        size_t full_blocks = remaining / block_size;
        if (full_blocks > 0xffffU) {
            full_blocks = 0xffffU;
        }

        if (lba64 > 0xffffffffULL) {
            goto done;
        }

        if (!_msc_rw10(lun, (u32)lba64, (u16)full_blocks, out, false)) {
            goto done;
        }

        size_t chunk = full_blocks * block_size;
        out += chunk;
        remaining -= chunk;
        lba64 += full_blocks;
    }

    if (remaining) {
        if (!bounce) {
            bounce = malloc(block_size);
            if (!bounce) {
                goto done;
            }
        }

        if (lba64 > 0xffffffffULL) {
            goto done;
        }

        if (!_msc_rw10(lun, (u32)lba64, 1, bounce, false)) {
            goto done;
        }

        memcpy(out, bounce, remaining);
    }

    ret = (ssize_t)bytes;

done:
    free(bounce);
    mutex_unlock(&lun->io_lock);
    return ret;
}

static ssize_t _usb_msc_write(disk_dev_t *disk, void *src, size_t offset, size_t bytes) {
    if (!disk || !src || !disk->private) {
        return -1;
    }

    usb_msc_lun_t *lun = disk->private;

    if (!lun->online || !lun->block_size || !lun->block_count) {
        return -1;
    }

    if (lun->block_count > ((size_t)-1 / lun->block_size)) {
        return -1;
    }

    size_t disk_size = lun->block_count * lun->block_size;

    if (offset >= disk_size) {
        return 0;
    }

    if (bytes > disk_size - offset) {
        bytes = disk_size - offset;
    }

    if (!bytes) {
        return 0;
    }

    mutex_lock(&lun->io_lock);

    ssize_t ret = -1;
    size_t block_size = lun->block_size;
    u8 *in = src;
    size_t remaining = bytes;
    u64 lba64 = offset / block_size;
    size_t block_off = offset % block_size;

    u8 *bounce = NULL;
    if (block_off || (remaining && remaining < block_size)) {
        bounce = malloc(block_size);
        if (!bounce) {
            goto done;
        }
    }

    while (remaining) {
        if (lba64 > 0xffffffffULL) {
            goto done;
        }

        if (block_off || remaining < block_size) {
            size_t chunk = block_size - block_off;
            if (chunk > remaining) {
                chunk = remaining;
            }

            if (!_msc_rw10(lun, (u32)lba64, 1, bounce, false)) {
                goto done;
            }

            memcpy(bounce + block_off, in, chunk);

            if (!_msc_rw10(lun, (u32)lba64, 1, bounce, true)) {
                goto done;
            }

            in += chunk;
            remaining -= chunk;
            lba64++;
            block_off = 0;
            continue;
        }

        size_t full_blocks = remaining / block_size;
        if (full_blocks > 0xffffU) {
            full_blocks = 0xffffU;
        }

        if (!_msc_rw10(lun, (u32)lba64, (u16)full_blocks, in, true)) {
            goto done;
        }

        size_t chunk = full_blocks * block_size;
        in += chunk;
        remaining -= chunk;
        lba64 += full_blocks;
    }

    ret = (ssize_t)bytes;

done:
    free(bounce);
    mutex_unlock(&lun->io_lock);
    return ret;
}

static usb_msc_lun_t *_msc_find_slot_locked(size_t hcd_id, size_t port, u8 lun) {
    for (size_t i = 0; i < ARRAY_LEN(msc_luns); i++) {
        usb_msc_lun_t *slot = &msc_luns[i];
        if (!slot->used) {
            continue;
        }

        if (slot->hcd_id == hcd_id && slot->port == port && slot->lun == lun) {
            return slot;
        }
    }

    return NULL;
}

static usb_msc_lun_t *_msc_alloc_slot_locked(size_t hcd_id, size_t port, u8 lun) {
    usb_msc_lun_t *slot = _msc_find_slot_locked(hcd_id, port, lun);
    if (slot) {
        return slot;
    }

    for (size_t i = 0; i < ARRAY_LEN(msc_luns); i++) {
        slot = &msc_luns[i];
        if (slot->used) {
            continue;
        }

        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->hcd_id = hcd_id;
        slot->port = port;
        slot->lun = lun;
        mutex_init(&slot->io_lock);
        return slot;
    }

    return NULL;
}

static bool _msc_register_disk(usb_msc_lun_t *lun) {
    if (!lun) {
        return false;
    }

    if (lun->disk) {
        lun->disk->sector_size = lun->block_size;
        lun->disk->sector_count = lun->block_count;
        return true;
    }

    static disk_interface_t msc_interface = {
        .read = _usb_msc_read,
        .write = _usb_msc_write,
    };

    disk_dev_t *disk = calloc(1, sizeof(disk_dev_t));
    if (!disk) {
        return false;
    }

    disk->type = DISK_USB;
    disk->sector_size = lun->block_size;
    disk->sector_count = lun->block_count;
    disk->interface = &msc_interface;
    disk->private = lun;

    if (!disk_register(disk)) {
        free(disk);
        return false;
    }

    lun->disk = disk;

    if (sched_is_running()) {
        disk_publish_devices();
    }

    return true;
}

static bool _msc_unpublish_lun(usb_msc_lun_t *lun) {
    if (!lun || !lun->disk) {
        return true;
    }

    if (disk_is_busy(lun->disk)) {
        return false;
    }

    if (!disk_unregister(lun->disk)) {
        return false;
    }

    free(lun->disk->name);
    free(lun->disk);
    lun->disk = NULL;
    return true;
}

static u8 _msc_get_max_lun(usb_device_handle_t dev, u8 interface_number) {
    usb_setup_packet_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE,
        .request = USB_MSC_GET_MAX_LUN,
        .value = 0,
        .index = interface_number,
        .length = 1,
    };

    u8 max_lun = 0;
    size_t actual = 0;

    if (
        !usb_device_control_transfer(
            dev,
            &setup,
            &max_lun,
            1,
            USB_MSC_CMD_TIMEOUT_MS,
            &actual
        ) ||
        actual != 1
    ) {
        return 0;
    }

    return max_lun;
}

static bool _usb_msc_attach(usb_device_handle_t dev) {
    size_t hcd_id = usb_device_hcd_id(dev);
    size_t port = usb_device_port(dev);
    const usb_device_identity_t *identity = usb_device_identity(dev);

    if (!identity) {
        return false;
    }

    u8 subclass =
        identity->interface_class ? identity->interface_subclass : identity->device_subclass;
    u8 protocol =
        identity->interface_class ? identity->interface_protocol : identity->device_protocol;

    if (!_msc_subclass_supported(subclass)) {
        log_warn("USB MSC hcd=%zu port=%zu unsupported subclass=%#x", hcd_id, port, subclass);
        return false;
    }

    if (!_msc_protocol_supported(protocol)) {
        log_warn("USB MSC hcd=%zu port=%zu unsupported protocol=%#x", hcd_id, port, protocol);
        return false;
    }

    if (!identity->bulk_in_ep || !identity->bulk_out_ep) {
        log_warn("USB MSC hcd=%zu port=%zu missing bulk endpoints", hcd_id, port);
        return false;
    }

    if (
        protocol == USB_MSC_PROTO_UAS &&
        identity->bulk_in_ep &&
        identity->bulk_out_ep
    ) {
        log_info("USB MSC hcd=%zu port=%zu protocol=UAS using BOT transport", hcd_id, port);
    } else if (protocol == USB_MSC_PROTO_CBI || protocol == USB_MSC_PROTO_CBI_NOINTR) {
        log_info("USB MSC hcd=%zu port=%zu protocol=CBI using control/bulk", hcd_id, port);
    }

    u8 max_lun = 0;
    if (
        protocol == USB_MSC_PROTO_BOT ||
        protocol == USB_MSC_PROTO_UAS
    ) {
        max_lun = _msc_get_max_lun(dev, identity->interface_number);
    }

    if (max_lun > 15) {
        max_lun = 15;
    }

    size_t attached = 0;

    for (u8 lun_id = 0; lun_id <= max_lun; lun_id++) {
        unsigned long state_flags = spin_lock_irqsave(&msc_state_lock);

        usb_msc_lun_t *lun = _msc_alloc_slot_locked(hcd_id, port, lun_id);
        if (!lun) {
            spin_unlock_irqrestore(&msc_state_lock, state_flags);
            log_warn("USB MSC table full, dropping hcd=%zu port=%zu lun=%u", hcd_id, port, lun_id);
            continue;
        }

        lun->interface_number = identity->interface_number;
        lun->bulk_in_ep = identity->bulk_in_ep;
        lun->bulk_out_ep = identity->bulk_out_ep;
        lun->device = dev;
        lun->subclass = subclass;
        lun->protocol = protocol;
        lun->transport = (u8)_msc_transport_for_protocol(protocol);
        lun->online = true;

        spin_unlock_irqrestore(&msc_state_lock, state_flags);

        if (!_msc_inquiry(lun)) {
            log_debug(
                "USB MSC hcd=%zu port=%zu lun=%u failed while reading inquiry data",
                hcd_id,
                port,
                lun_id
            );

            state_flags = spin_lock_irqsave(&msc_state_lock);
            lun->online = false;
            spin_unlock_irqrestore(&msc_state_lock, state_flags);

            continue;
        }

        _msc_test_unit_ready(lun);

        size_t blocks = 0;
        size_t block_size = 0;

        if (!_msc_read_capacity10(lun, &blocks, &block_size)) {
            log_debug(
                "USB MSC hcd=%zu port=%zu lun=%u failed while reading capacity",
                hcd_id,
                port,
                lun_id
            );

            state_flags = spin_lock_irqsave(&msc_state_lock);
            lun->online = false;
            spin_unlock_irqrestore(&msc_state_lock, state_flags);

            continue;
        }

        if (!blocks || !block_size) {
            log_debug(
                "USB MSC hcd=%zu port=%zu lun=%u invalid capacity blocks=%zu block=%zu",
                hcd_id,
                port,
                lun_id,
                blocks,
                block_size
            );

            state_flags = spin_lock_irqsave(&msc_state_lock);
            lun->online = false;
            spin_unlock_irqrestore(&msc_state_lock, state_flags);

            continue;
        }

        state_flags = spin_lock_irqsave(&msc_state_lock);
        bool still_online =
            lun->used &&
            lun->hcd_id == hcd_id &&
            lun->port == port &&
            lun->lun == lun_id &&
            lun->online;
        if (still_online) {
            lun->block_size = block_size;
            lun->block_count = blocks;
        }
        spin_unlock_irqrestore(&msc_state_lock, state_flags);

        if (!still_online) {
            continue;
        }

        if (!_msc_register_disk(lun)) {
            log_warn(
                "USB MSC hcd=%zu port=%zu lun=%u failed while registering disk",
                hcd_id,
                port,
                lun_id
            );

            state_flags = spin_lock_irqsave(&msc_state_lock);
            lun->online = false;
            spin_unlock_irqrestore(&msc_state_lock, state_flags);

            continue;
        }

        attached++;

        log_info(
            "USB MSC hcd=%zu port=%zu lun=%u online sectors=%zu block=%zu",
            hcd_id,
            port,
            lun_id,
            lun->block_count,
            lun->block_size
        );
    }

    if (!attached) {
        log_warn("USB MSC attached on hcd=%zu port=%zu but no accessible LUNs", hcd_id, port);
    }

    return attached != 0;
}

static void _usb_msc_detach(usb_device_handle_t dev) {
    size_t hcd_id = usb_device_hcd_id(dev);
    size_t port = usb_device_port(dev);

    size_t detached_indexes[USB_MSC_MAX_LUNS] = {0};
    size_t detached = 0;

    unsigned long state_flags = spin_lock_irqsave(&msc_state_lock);

    for (size_t i = 0; i < ARRAY_LEN(msc_luns); i++) {
        usb_msc_lun_t *lun = &msc_luns[i];

        if (!lun->used) {
            continue;
        }

        if (lun->hcd_id != hcd_id || lun->port != port) {
            continue;
        }

        lun->online = false;
        lun->device = NULL;
        detached_indexes[detached] = i;
        detached++;
    }

    spin_unlock_irqrestore(&msc_state_lock, state_flags);

    size_t released = 0;
    size_t busy = 0;

    for (size_t i = 0; i < detached; i++) {
        usb_msc_lun_t *lun = &msc_luns[detached_indexes[i]];
        if (_msc_unpublish_lun(lun)) {
            released++;
            continue;
        }

        busy++;
    }

    state_flags = spin_lock_irqsave(&msc_state_lock);
    for (size_t i = 0; i < detached; i++) {
        usb_msc_lun_t *lun = &msc_luns[detached_indexes[i]];
        if (!lun->disk) {
            memset(lun, 0, sizeof(*lun));
        }
    }
    spin_unlock_irqrestore(&msc_state_lock, state_flags);

    if (detached) {
        log_info(
            "USB MSC detached hcd=%zu port=%zu luns=%zu released=%zu busy=%zu",
            hcd_id,
            port,
            detached,
            released,
            busy
        );
    }
}

static bool usb_msc_init(void) {
    if (msc_registered) {
        return true;
    }

    static const usb_class_driver_t msc_driver = {
        .name = "usb-msc",
        .match = _usb_msc_match,
        .attach = _usb_msc_attach,
        .detach = _usb_msc_detach,
    };

    if (!usb_register_class_driver(&msc_driver)) {
        return false;
    }

    msc_registered = true;
    return true;
}

bool usb_msc_driver_busy(void) {
    for (size_t i = 0; i < ARRAY_LEN(msc_luns); i++) {
        usb_msc_lun_t *lun = &msc_luns[i];
        if (lun->used && lun->disk && disk_is_busy(lun->disk)) {
            return true;
        }
    }

    return false;
}

driver_err_t usb_msc_driver_load(void) {
    if (msc_registered) {
        return DRIVER_OK;
    }

    if (!usb_core_is_ready()) {
        return DRIVER_ERR_DEPENDENCY;
    }

    if (!usb_msc_init()) {
        return DRIVER_ERR_INIT_FAILED;
    }

    return DRIVER_OK;
}

driver_err_t usb_msc_driver_unload(void) {
    if (!msc_registered) {
        return DRIVER_OK;
    }

    if (usb_msc_driver_busy()) {
        return DRIVER_ERR_BUSY;
    }

    if (!usb_unregister_class_driver("usb-msc")) {
        log_warn("USB MSC class driver unregister failed");
    }

    for (size_t i = 0; i < ARRAY_LEN(msc_luns); i++) {
        usb_msc_lun_t *lun = &msc_luns[i];
        if (!lun->used) {
            continue;
        }

        lun->online = false;
        lun->device = NULL;

        if (!_msc_unpublish_lun(lun)) {
            return DRIVER_ERR_BUSY;
        }

        memset(lun, 0, sizeof(*lun));
    }

    msc_registered = false;
    return DRIVER_OK;
}
