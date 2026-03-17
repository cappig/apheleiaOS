#include "usb_xhci_internal.h"

#include <log/log.h>

bool _xhci_poll_events(
    xhci_controller_t *ctrl,
    bool process_port_events,
    bool force_event_scan
) {
    if (!ctrl || !ctrl->used) {
        return false;
    }

    if (!spin_try_lock(&ctrl->event_lock)) {
        return false;
    }

    void *map = arch_phys_map(ctrl->mmio_base, XHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (!map) {
        spin_unlock(&ctrl->event_lock);
        return false;
    }

    volatile u8 *op = (volatile u8 *)map + ctrl->cap_length;
    u32 status = _read32(op, XHCI_OP_USBSTS_OFF);

    bool progressed = false;

    if ((status & XHCI_USBSTS_EINT) || force_event_scan) {
        progressed |= _xhci_process_events(ctrl, map, op, process_port_events);
    }

    if (status & XHCI_USBSTS_PCD) {
        if (process_port_events) {
            (void)_xhci_scan_ports(ctrl, op, true, false);
        }

        progressed = true;
    }

    if (status & XHCI_USBSTS_HSE) {
        _xhci_log_fault_snapshot(ctrl, "USBSTS.HSE asserted");
        ctrl->commands_healthy = false;
        _xhci_set_health_state(ctrl, XHCI_HEALTH_DEGRADED, "USBSTS.HSE asserted");
    }

    if (ctrl->runtime_ready && _xhci_runtime_available(ctrl)) {
        volatile u8 *ir = (volatile u8 *)map + ctrl->rt_offset + XHCI_RT_IR0_OFF;
        u32 iman = _read32(ir, XHCI_IR_IMAN_OFF);

        if (iman & XHCI_IMAN_IP) {
            _write32(ir, XHCI_IR_IMAN_OFF, iman | XHCI_IMAN_IP);
            progressed = true;
        }
    }

    u32 ack = status & (XHCI_USBSTS_EINT | XHCI_USBSTS_PCD);
    if (ack) {
        _write32(op, XHCI_OP_USBSTS_OFF, ack);
        progressed = true;
    }

    arch_phys_unmap(map, XHCI_MMIO_SIZE);
    spin_unlock(&ctrl->event_lock);

    if (progressed) {
        unsigned long flags = arch_irq_save();
        ctrl->irq_seq++;
        arch_irq_restore(flags);
    }

    return progressed;
}

bool _xhci_process_events(
    xhci_controller_t *ctrl,
    volatile void *mmio,
    volatile u8 *op,
    bool process_port_events
) {
    if (!ctrl || !mmio || !op || !ctrl->runtime_ready || !ctrl->event_ring_trbs) {
        return false;
    }

    void *ring_map = arch_phys_map(ctrl->event_ring_paddr, XHCI_DMA_BYTES, 0);
    if (!ring_map) {
        return false;
    }

    bool progressed = false;
    xhci_trb_t *ring = ring_map;
    size_t index = ctrl->event_dequeue;
    u8 cycle = ctrl->event_cycle;
    size_t processed = 0;

    while (processed < ctrl->event_ring_trbs) {
        xhci_trb_t trb = ring[index];
        bool cycle_bit = (trb.control & XHCI_TRB_CYCLE) != 0;

        if (cycle_bit != (cycle != 0)) {
            if (!processed && ctrl->event_cycle_sync_pending) {
                u32 type = _trb_type(&trb);

                if (type >= XHCI_TRB_TYPE_TRANSFER_EVENT) {
                    cycle ^= 1U;
                    ctrl->event_cycle_sync_pending = false;

                    log_debug(
                        "xHCI %u:%u.%u event cycle bootstrap resync",
                        ctrl->bus,
                        ctrl->slot,
                        ctrl->func
                    );
                    continue;
                }
            }

            break;
        }

        u32 type = _trb_type(&trb);
        if (type < XHCI_TRB_TYPE_TRANSFER_EVENT) {
            if (!processed && ctrl->event_cycle_sync_pending) {
                cycle ^= 1U;
                ctrl->event_cycle_sync_pending = false;

                log_debug(
                    "xHCI %u:%u.%u event cycle resync after invalid type=%u",
                    ctrl->bus,
                    ctrl->slot,
                    ctrl->func,
                    (unsigned int)type
                );
                continue;
            }

            break;
        }

        if (type == XHCI_TRB_TYPE_PORT_STATUS_CHANGE) {
            if (process_port_events) {
                size_t port = (size_t)((trb.parameter_lo >> 24) & 0xffU);

                if (port) {
                    _xhci_report_port(ctrl, op, port, true, false);
                } else {
                    (void)_xhci_scan_ports(ctrl, op, true, false);
                }
            }
        } else if (type == XHCI_TRB_TYPE_CMD_COMPLETION) {
            u8 cc = (u8)((trb.status >> 24) & 0xffU);
            u8 slot = (u8)((trb.control >> XHCI_TRB_SLOT_ID_SHIFT) & 0xffU);
            u64 ptr = _trb_param(&trb);

            bool matched = false;
            unsigned long flags = arch_irq_save();

            if (
                ctrl->cmd_wait_active &&
                (
                    ptr == ctrl->cmd_wait_trb ||
                    (ptr & ~0x0fULL) == (ctrl->cmd_wait_trb & ~0x0fULL) ||
                    ptr == 0
                )
            ) {
                ctrl->cmd_wait_cc = cc;
                ctrl->cmd_wait_slot = slot;
                ctrl->cmd_wait_done = true;
                matched = true;
            }

            if (!matched && ctrl->cmd_wait_active) {
                // some controllers do not report the exact command TRB pointer in
                // completion events.. since command submission is serialized, the
                // active waiter can safely consume the completion
                ctrl->cmd_wait_cc = cc;
                ctrl->cmd_wait_slot = slot;
                ctrl->cmd_wait_done = true;
                matched = true;
            }

            arch_irq_restore(flags);

            if (!matched && cc != XHCI_CC_SUCCESS) {
                log_warn(
                    "xHCI %u:%u.%u command completion cc=%#x slot=%u",
                    ctrl->bus,
                    ctrl->slot,
                    ctrl->func,
                    cc,
                    slot
                );
            }
        } else if (type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
            u8 cc = (u8)((trb.status >> 24) & 0xffU);
            u32 residual = trb.status & 0x00ffffffU;
            u8 ep = (u8)((trb.control >> XHCI_TRB_EP_ID_SHIFT) & 0x1fU);
            u8 slot = (u8)((trb.control >> XHCI_TRB_SLOT_ID_SHIFT) & 0xffU);
            u64 ptr = _trb_param(&trb);

            bool matched = false;
            unsigned long flags = arch_irq_save();

            u64 wait_ptr = ctrl->xfer_wait_trb;
            u64 wait_ptr_aligned = wait_ptr & ~0x0fULL;
            u64 ptr_aligned = ptr & ~0x0fULL;

            bool ep_slot_match =
                ctrl->xfer_wait_active &&
                slot == ctrl->xfer_wait_slot &&
                ep == ctrl->xfer_wait_ep;
            bool ptr_match =
                ptr == wait_ptr ||
                ptr_aligned == wait_ptr_aligned ||
                ptr == 0;

            if (ep_slot_match && ptr_match) {
                ctrl->xfer_wait_cc = cc;
                ctrl->xfer_wait_residual = residual;
                ctrl->xfer_wait_done = true;
                matched = true;
            }

            if (!matched && ep_slot_match) {
                ctrl->xfer_wait_cc = cc;
                ctrl->xfer_wait_residual = residual;
                ctrl->xfer_wait_done = true;
                matched = true;
            }

            arch_irq_restore(flags);

            if (
                !matched &&
                cc != XHCI_CC_SUCCESS &&
                cc != XHCI_CC_SHORT_PACKET
            ) {
                log_warn(
                    "xHCI %u:%u.%u transfer completion cc=%#x slot=%u ep=%u",
                    ctrl->bus,
                    ctrl->slot,
                    ctrl->func,
                    cc,
                    slot,
                    ep
                );
            }
        }

        progressed = true;
        processed++;
        index++;

        if (index >= ctrl->event_ring_trbs) {
            index = 0;
            cycle ^= 1U;
        }
    }

    ctrl->event_dequeue = (u16)index;
    ctrl->event_cycle = cycle;

    if (processed) {
        ctrl->event_cycle_sync_pending = false;
    }

    arch_phys_unmap(ring_map, XHCI_DMA_BYTES);
    if (!_xhci_update_erdp(ctrl, mmio)) {
        log_debug(
            "xHCI %u:%u.%u ERDP readback mismatch (continuing)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func
        );
    }

    return progressed;
}

void _xhci_service_interrupts(void) {
    for (size_t i = 0; i < controller_count; i++) {
        xhci_controller_t *ctrl = &controllers[i];

        if (!ctrl->used || !ctrl->irq_enabled) {
            continue;
        }

        (void)_xhci_poll_events(ctrl, true, false);
    }
}
