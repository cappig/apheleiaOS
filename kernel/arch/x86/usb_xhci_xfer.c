#include "usb_xhci_internal.h"

#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "sys/usb_desc.h"
#include "usb_hcd_common.h"

bool _xhci_control_xfer(
    xhci_usb_device_t *dev,
    const usb_setup_packet_t *setup,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
) {
    if (!dev || !dev->ctrl || !setup || !dev->slot_id || !dev->ep0_ring.paddr) {
        return false;
    }

    xhci_controller_t *ctrl = dev->ctrl;

    bool data_in = (setup->request_type & USB_REQ_DIR_IN) != 0;

    if (length && !buffer) {
        return false;
    }

    size_t dma_pages = 0;
    u64 dma_paddr = 0;
    void *dma_map = NULL;

    if (length) {
        dma_pages = DIV_ROUND_UP(length, XHCI_DMA_BYTES);
        if (!_xhci_alloc_dma_pages(ctrl, dma_pages, &dma_paddr)) {
            return false;
        }

        dma_map = arch_phys_map(dma_paddr, dma_pages * XHCI_DMA_BYTES, 0);
        if (!dma_map) {
            _xhci_free_dma_pages(&dma_paddr, dma_pages);
            return false;
        }

        if (!data_in) {
            memcpy(dma_map, buffer, length);
        }
    }

    xhci_trb_t trbs[3] = {0};
    size_t trb_count = 0;

    u64 setup_data =
        (u64)setup->request_type |
        ((u64)setup->request << 8) |
        ((u64)setup->value << 16) |
        ((u64)setup->index << 32) |
        ((u64)setup->length << 48);

    u32 trt = XHCI_SETUP_TRT_NONE;
    if (length) {
        trt = data_in ? XHCI_SETUP_TRT_IN : XHCI_SETUP_TRT_OUT;
    }

    trbs[trb_count++] = (xhci_trb_t){
        .parameter_lo = (u32)(setup_data & 0xffffffffULL),
        .parameter_hi = (u32)(setup_data >> 32),
        .status = 8,
        .control =
            (XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_IDT |
            (trt << XHCI_SETUP_TRT_SHIFT),
    };

    if (length) {
        trbs[trb_count++] = (xhci_trb_t){
            .parameter_lo = (u32)(dma_paddr & 0xffffffffULL),
            .parameter_hi = (u32)(dma_paddr >> 32),
            .status = (u32)length,
            .control =
                (XHCI_TRB_TYPE_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) |
                XHCI_TRB_CHAIN |
                (data_in ? (1U << 16) : 0),
        };
    }

    trbs[trb_count++] = (xhci_trb_t){
        .parameter_lo = 0,
        .parameter_hi = 0,
        .status = 0,
        .control =
            (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_IOC |
            (data_in ? 0 : (1U << 16)),
    };

    bool ok = _xhci_submit_transfer(
        ctrl,
        dev,
        &dev->ep0_ring,
        1,
        trbs,
        trb_count,
        trb_count - 1,
        length,
        true,
        timeout_ms ? timeout_ms : XHCI_CMD_TIMEOUT_MS,
        out_actual
    );

    if (ok && length && data_in) {
        size_t copy_len = length;
        if (out_actual && *out_actual < copy_len) {
            copy_len = *out_actual;
        }
        memcpy(buffer, dma_map, copy_len);
    }

    if (!ok && out_actual) {
        *out_actual = 0;
    }

    if (dma_map) {
        arch_phys_unmap(dma_map, dma_pages * XHCI_DMA_BYTES);
    }

    if (dma_paddr) {
        _xhci_free_dma_pages(&dma_paddr, dma_pages);
    }

    return ok;
}

u16 _xhci_default_bulk_mps(const xhci_usb_device_t *dev) {
    if (!dev) {
        return 64;
    }

    if (dev->port_speed_id >= 4) {
        return 1024;
    }

    if (dev->port_speed_id >= 3) {
        return 512;
    }

    return 64;
}

bool _xhci_ensure_bulk_endpoint(xhci_usb_device_t *dev, u8 endpoint) {
    if (!dev || !dev->ctrl || !(endpoint & USB_ENDPOINT_NUM_MASK)) {
        return false;
    }

    bool in_dir = (endpoint & USB_ENDPOINT_DIR_IN) != 0;
    u8 dci = _ep_to_dci(endpoint);
    if (!dci) {
        return false;
    }

    if (in_dir && dev->bulk_in_ready && dev->bulk_in_ep == endpoint) {
        return true;
    }

    if (!in_dir && dev->bulk_out_ready && dev->bulk_out_ep == endpoint) {
        return true;
    }

    if (in_dir && dev->bulk_in_ready && dev->bulk_in_ep != endpoint) {
        return false;
    }

    if (!in_dir && dev->bulk_out_ready && dev->bulk_out_ep != endpoint) {
        return false;
    }

    xhci_controller_t *ctrl = dev->ctrl;
    u64 *ring_paddr = in_dir ? &dev->bulk_in_ring.paddr : &dev->bulk_out_ring.paddr;
    xhci_ring_state_t *ring = in_dir ? &dev->bulk_in_ring : &dev->bulk_out_ring;
    u8 *stored_ep = in_dir ? &dev->bulk_in_ep : &dev->bulk_out_ep;
    u8 *stored_dci = in_dir ? &dev->bulk_in_dci : &dev->bulk_out_dci;
    u16 *stored_mps = in_dir ? &dev->bulk_in_mps : &dev->bulk_out_mps;
    bool *ready = in_dir ? &dev->bulk_in_ready : &dev->bulk_out_ready;

    if (!*ring_paddr && !_xhci_alloc_dma_pages(ctrl, 1, ring_paddr)) {
        return false;
    }

    if (!ring->trbs && !_xhci_ring_init(ctrl, ring, *ring_paddr)) {
        _xhci_free_dma_pages(ring_paddr, 1);
        return false;
    }

    *stored_ep = endpoint;
    *stored_dci = dci;
    if (!*stored_mps) {
        *stored_mps = _xhci_default_bulk_mps(dev);
    }
    *ready = true;

    if (
        !_xhci_prepare_config_ctx(dev) ||
        !_xhci_cmd_configure_endpoint(ctrl, dev->slot_id, dev->input_ctx_paddr)
    ) {
        *ready = false;
        *stored_ep = 0;
        *stored_dci = 0;
        *stored_mps = 0;
        ring->trbs = 0;
        ring->enqueue = 0;
        ring->cycle = 0;
        _xhci_free_dma_pages(ring_paddr, 1);
        return false;
    }

    delay_ms(2);
    return true;
}

bool _xhci_bulk_xfer(
    xhci_usb_device_t *dev,
    u8 endpoint,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
) {
    if (!dev || !dev->ctrl) {
        return false;
    }

    if (length && !buffer) {
        return false;
    }

    if (!length) {
        if (out_actual) {
            *out_actual = 0;
        }
        return true;
    }

    xhci_controller_t *ctrl = dev->ctrl;
    if (!_xhci_ensure_bulk_endpoint(dev, endpoint)) {
        return false;
    }

    xhci_ring_state_t *ring = NULL;
    u8 ep_id = 0;

    if (dev->bulk_in_ready && endpoint == dev->bulk_in_ep) {
        ring = &dev->bulk_in_ring;
        ep_id = dev->bulk_in_dci;
    } else if (dev->bulk_out_ready && endpoint == dev->bulk_out_ep) {
        ring = &dev->bulk_out_ring;
        ep_id = dev->bulk_out_dci;
    } else {
        return false;
    }

    bool data_in = (endpoint & USB_ENDPOINT_DIR_IN) != 0;

    u8 *cursor = buffer;
    size_t done = 0;

    while (done < length) {
        size_t chunk = length - done;
        if (chunk > 65536U) {
            chunk = 65536U;
        }

        size_t dma_pages = DIV_ROUND_UP(chunk, XHCI_DMA_BYTES);
        u64 dma_paddr = 0;

        if (!_xhci_alloc_dma_pages(ctrl, dma_pages, &dma_paddr)) {
            if (out_actual) {
                *out_actual = done;
            }
            return false;
        }

        void *dma_map = arch_phys_map(dma_paddr, dma_pages * XHCI_DMA_BYTES, 0);
        if (!dma_map) {
            _xhci_free_dma_pages(&dma_paddr, dma_pages);

            if (out_actual) {
                *out_actual = done;
            }

            return false;
        }

        if (!data_in) {
            memcpy(dma_map, cursor, chunk);
        }

        xhci_trb_t trb = {
            .parameter_lo = (u32)(dma_paddr & 0xffffffffULL),
            .parameter_hi = (u32)(dma_paddr >> 32),
            .status = (u32)chunk,
            .control =
                (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) |
                XHCI_TRB_IOC,
        };

        size_t chunk_actual = 0;
        bool ok = _xhci_submit_transfer(
            ctrl,
            dev,
            ring,
            ep_id,
            &trb,
            1,
            0,
            chunk,
            data_in,
            timeout_ms ? timeout_ms : XHCI_XFER_TIMEOUT_MS,
            &chunk_actual
        );

        if (ok && data_in && chunk_actual) {
            memcpy(cursor, dma_map, chunk_actual);
        }

        arch_phys_unmap(dma_map, dma_pages * XHCI_DMA_BYTES);
        _xhci_free_dma_pages(&dma_paddr, dma_pages);

        if (!ok) {
            if (out_actual) {
                *out_actual = done;
            }
            return false;
        }

        done += chunk_actual;
        cursor += chunk_actual;

        if (chunk_actual < chunk) {
            break;
        }
    }

    if (out_actual) {
        *out_actual = done;
    }

    return true;
}

void _xhci_free_usb_device(xhci_usb_device_t *dev) {
    if (!dev) {
        return;
    }

    _xhci_free_dma_pages(&dev->input_ctx_paddr, 1);
    _xhci_free_dma_pages(&dev->output_ctx_paddr, 1);

    _xhci_free_dma_pages(&dev->ep0_ring.paddr, 1);
    _xhci_free_dma_pages(&dev->bulk_in_ring.paddr, 1);
    _xhci_free_dma_pages(&dev->bulk_out_ring.paddr, 1);

    free(dev);
}

void _xhci_release_usb_device(
    xhci_controller_t *ctrl,
    size_t port,
    xhci_usb_device_t *dev
) {
    if (!ctrl || !dev) {
        return;
    }

    if (dev->slot_id) {
        (void)_xhci_cmd_disable_slot(ctrl, dev->slot_id);
        (void)_xhci_dcbaa_set_entry(ctrl, dev->slot_id, 0);
    }

    if (port && port <= XHCI_MAX_SCAN_PORTS && ctrl->port_devices[port] == dev) {
        ctrl->port_devices[port] = NULL;
    }

    _xhci_free_usb_device(dev);
}

void _xhci_release_device(size_t hcd_id, size_t port, void *device_ctx) {
    if (!hcd_id || !device_ctx) {
        return;
    }

    xhci_controller_t *ctrl = _xhci_find_by_hcd(hcd_id);
    if (!ctrl) {
        return;
    }

    xhci_usb_device_t *dev = device_ctx;

    if (dev->ctrl != ctrl) {
        return;
    }

    if (!port) {
        port = dev->port;
    }

    _xhci_release_usb_device(ctrl, port, dev);
}

bool _xhci_hcd_control_transfer(
    size_t hcd_id,
    size_t port,
    void *device_ctx,
    const usb_transfer_t *transfer,
    size_t *out_actual
) {
    (void)hcd_id;
    (void)port;

    if (!device_ctx || !transfer || !transfer->setup) {
        return false;
    }

    xhci_usb_device_t *dev = device_ctx;
    const usb_setup_packet_t *setup = transfer->setup;

    bool ok = _xhci_control_xfer(
        dev,
        setup,
        transfer->buffer,
        transfer->length,
        transfer->timeout_ms,
        out_actual
    );

    if (!ok) {
        return false;
    }

    if (
        setup->request == USB_REQ_GET_DESCRIPTOR &&
        (setup->request_type & USB_REQ_DIR_IN) &&
        (((setup->value >> 8) & 0xffU) == USB_DT_CONFIG) &&
        transfer->buffer
    ) {
        size_t cfg_len = transfer->length;
        if (out_actual && *out_actual < cfg_len) {
            cfg_len = *out_actual;
        }

        if (cfg_len >= 9) {
            usb_device_identity_t identity = {
                .device_class = dev->device_class,
                .device_subclass = dev->device_subclass,
                .device_protocol = dev->device_protocol,
                .vendor_id = dev->vendor_id,
                .product_id = dev->product_id,
            };

            u8 config_value = 0;
            if (
                usb_desc_parse_identity_from_config(
                    transfer->buffer,
                    cfg_len,
                    &identity,
                    &config_value
                )
            ) {
                dev->config_value = config_value;
                dev->interface_number = identity.interface_number;
                dev->interface_class = identity.interface_class;
                dev->interface_subclass = identity.interface_subclass;
                dev->interface_protocol = identity.interface_protocol;

                dev->bulk_in_ep = identity.bulk_in_ep;
                dev->bulk_out_ep = identity.bulk_out_ep;
                dev->bulk_in_mps = identity.bulk_in_max_packet;
                dev->bulk_out_mps = identity.bulk_out_max_packet;
                dev->bulk_in_dci = _ep_to_dci(dev->bulk_in_ep);
                dev->bulk_out_dci = _ep_to_dci(dev->bulk_out_ep);
            }
        }
    }

    if (
        setup->request == USB_REQ_SET_CONFIGURATION &&
        !(setup->request_type & USB_REQ_DIR_IN)
    ) {
        if (setup->value) {
            dev->config_value = (u8)setup->value;
        }

        if (dev->bulk_out_ep && !_xhci_ensure_bulk_endpoint(dev, dev->bulk_out_ep)) {
            return false;
        }

        if (dev->bulk_in_ep && !_xhci_ensure_bulk_endpoint(dev, dev->bulk_in_ep)) {
            return false;
        }
    }

    return true;
}

bool _xhci_hcd_bulk_transfer(
    size_t hcd_id,
    size_t port,
    void *device_ctx,
    const usb_transfer_t *transfer,
    size_t *out_actual
) {
    (void)hcd_id;
    (void)port;

    if (!device_ctx || !transfer) {
        return false;
    }

    xhci_usb_device_t *dev = device_ctx;

    return _xhci_bulk_xfer(
        dev,
        transfer->endpoint.address,
        transfer->buffer,
        transfer->length,
        transfer->timeout_ms,
        out_actual
    );
}

bool _xhci_hcd_port_reset(size_t hcd_id, size_t port, usb_speed_t *out_speed) {
    xhci_controller_t *ctrl = _xhci_find_by_hcd(hcd_id);
    if (!ctrl) {
        return false;
    }

    usb_speed_t speed = USB_SPEED_FULL;
    if (!_xhci_port_reset(ctrl, port, &speed)) {
        return false;
    }

    if (out_speed) {
        *out_speed = speed;
    }

    return true;
}

bool _xhci_hcd_device_open(
    size_t hcd_id,
    size_t port,
    usb_speed_t speed,
    void **out_device_ctx
) {
    if (!out_device_ctx) {
        return false;
    }

    xhci_controller_t *ctrl = _xhci_find_by_hcd(hcd_id);
    if (!ctrl || !port || port > ctrl->max_ports || port > XHCI_MAX_SCAN_PORTS) {
        return false;
    }

    if (ctrl->health_state == XHCI_HEALTH_OFFLINE) {
        return false;
    }

    if (!_xhci_ensure_running(ctrl)) {
        return false;
    }

    void *mmio_map = arch_phys_map(ctrl->mmio_base, XHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (!mmio_map) {
        return false;
    }

    volatile u8 *op = (volatile u8 *)mmio_map + ctrl->cap_length;

    size_t port_off = XHCI_OP_PORTSC_BASE + (port - 1) * XHCI_OP_PORTSC_STRIDE;
    u32 portsc = _read32(op, port_off);

    arch_phys_unmap(mmio_map, XHCI_MMIO_SIZE);

    if (!(portsc & XHCI_PORTSC_CCS)) {
        return false;
    }

    xhci_usb_device_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return false;
    }

    dev->ctrl = ctrl;
    dev->hcd_id = hcd_id;
    dev->port = port;
    dev->port_speed_id = (u8)((portsc >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK);

    if (!dev->port_speed_id) {
        dev->port_speed_id = _xhci_speed_to_psiv(speed);
    }

    if (dev->port_speed_id >= 4) {
        dev->max_packet0 = 512;
    } else if (dev->port_speed_id == 3) {
        dev->max_packet0 = 64;
    } else {
        dev->max_packet0 = 8;
    }

    if (
        !_xhci_alloc_dma_pages(ctrl, 1, &dev->input_ctx_paddr) ||
        !_xhci_alloc_dma_pages(ctrl, 1, &dev->output_ctx_paddr) ||
        !_xhci_alloc_dma_pages(ctrl, 1, &dev->ep0_ring.paddr)
    ) {
        _xhci_free_usb_device(dev);
        return false;
    }

    if (!_xhci_ring_init(ctrl, &dev->ep0_ring, dev->ep0_ring.paddr)) {
        _xhci_free_usb_device(dev);
        return false;
    }

    if (!_xhci_cmd_enable_slot(ctrl, &dev->slot_id) || !dev->slot_id) {
        _xhci_free_usb_device(dev);
        return false;
    }

    if (!_xhci_dcbaa_set_entry(ctrl, dev->slot_id, dev->output_ctx_paddr)) {
        _xhci_release_usb_device(ctrl, port, dev);
        return false;
    }

    if (
        !_xhci_prepare_address_ctx(dev) ||
        !_xhci_cmd_address_device(ctrl, dev->slot_id, dev->input_ctx_paddr)
    ) {
        _xhci_release_usb_device(ctrl, port, dev);
        return false;
    }

    delay_ms(10);

    if (ctrl->port_devices[port]) {
        _xhci_release_usb_device(ctrl, port, ctrl->port_devices[port]);
    }

    ctrl->port_devices[port] = dev;
    *out_device_ctx = dev;
    return true;
}

bool _xhci_hcd_set_address(
    size_t hcd_id,
    size_t port,
    void *device_ctx,
    u8 address,
    u16 ep0_mps
) {
    (void)hcd_id;
    (void)port;

    if (!device_ctx || !address) {
        return false;
    }

    xhci_usb_device_t *dev = device_ctx;
    xhci_controller_t *ctrl = dev->ctrl;
    if (!ctrl) {
        return false;
    }

    if (ep0_mps && ep0_mps != dev->max_packet0) {
        dev->max_packet0 = ep0_mps;

        if (
            !_xhci_prepare_ep0_eval_ctx(dev) ||
            !_xhci_cmd_evaluate_context(ctrl, dev->slot_id, dev->input_ctx_paddr)
        ) {
            return false;
        }

        delay_ms(2);
    }

    return true;
}

const usb_hcd_ops_t xhci_hcd_ops = {
    .port_reset = _xhci_hcd_port_reset,
    .device_open = _xhci_hcd_device_open,
    .device_close = _xhci_release_device,
    .set_address = _xhci_hcd_set_address,
    .control_transfer = _xhci_hcd_control_transfer,
    .bulk_transfer = _xhci_hcd_bulk_transfer,
};
