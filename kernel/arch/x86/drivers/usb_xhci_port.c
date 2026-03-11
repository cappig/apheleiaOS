#include "usb_xhci_internal.h"

#include <log/log.h>
#include <sys/time.h>

static usb_speed_t _xhci_speed_from_psiv(u32 psiv) {
    switch (psiv) {
    case 2:
        return USB_SPEED_LOW;
    case 3:
        return USB_SPEED_HIGH;
    case 4:
    case 5:
        return USB_SPEED_SUPER;
    case 1:
    default:
        return USB_SPEED_FULL;
    }
}

void _xhci_ack_port_change_bits(volatile u8 *op, size_t off, u32 portsc) {
    if (!op) {
        return;
    }

    u32 change = portsc & XHCI_PORTSC_CHANGE_BITS;
    if (!change) {
        return;
    }

    u32 write = (portsc & XHCI_PORTSC_PRESERVE_BITS) | change;
    _write32(op, off, write);
}

void _xhci_report_port(
    xhci_controller_t *ctrl,
    volatile u8 *op,
    size_t port,
    bool clear_changes,
    bool log_connected
) {
    if (!ctrl || !op || !port) {
        return;
    }

    if (port > ctrl->max_ports || port > XHCI_MAX_SCAN_PORTS) {
        return;
    }

    size_t off = XHCI_OP_PORTSC_BASE + (port - 1) * XHCI_OP_PORTSC_STRIDE;
    if (ctrl->cap_length + off + sizeof(u32) > XHCI_MMIO_SIZE) {
        return;
    }

    u32 portsc = _read32(op, off);
    bool connected = (portsc & XHCI_PORTSC_CCS) != 0;
    u32 psiv = (portsc >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;
    usb_speed_t speed = _xhci_speed_from_psiv(psiv);

    (void)usb_report_port_state(ctrl->hcd_id, port, connected, speed);

    if (clear_changes) {
        _xhci_ack_port_change_bits(op, off, portsc);
    }

    if (connected && log_connected) {
        log_info(
            "xHCI %u:%u.%u port %zu connected (speed=%u)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            port,
            (unsigned)psiv
        );
    }
}

size_t _xhci_scan_ports(
    xhci_controller_t *ctrl,
    volatile u8 *op,
    bool clear_changes,
    bool log_connected
) {
    if (!ctrl || !op) {
        return 0;
    }

    size_t connected = 0;
    size_t limit = ctrl->max_ports;

    if (limit > XHCI_MAX_SCAN_PORTS) {
        limit = XHCI_MAX_SCAN_PORTS;
    }

    for (size_t port = 1; port <= limit; port++) {
        size_t off = XHCI_OP_PORTSC_BASE + (port - 1) * XHCI_OP_PORTSC_STRIDE;
        if (ctrl->cap_length + off + sizeof(u32) > XHCI_MMIO_SIZE) {
            break;
        }

        u32 portsc = _read32(op, off);
        if (portsc & XHCI_PORTSC_CCS) {
            connected++;
        }

        _xhci_report_port(ctrl, op, port, clear_changes, log_connected);
    }

    return connected;
}

bool _xhci_port_reset(xhci_controller_t *ctrl, size_t port, usb_speed_t *out_speed) {
    if (!ctrl || !port || port > ctrl->max_ports) {
        return false;
    }

    void *map = arch_phys_map(ctrl->mmio_base, XHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (!map) {
        return false;
    }

    volatile u8 *op = (volatile u8 *)map + ctrl->cap_length;
    size_t off = XHCI_OP_PORTSC_BASE + (port - 1) * XHCI_OP_PORTSC_STRIDE;

    if (ctrl->cap_length + off + sizeof(u32) > XHCI_MMIO_SIZE) {
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    u32 portsc = _read32(op, off);
    if (!(portsc & XHCI_PORTSC_CCS)) {
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    u32 psiv = (portsc >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;
    bool superspeed = psiv >= 4;
    u32 write =
        (portsc & XHCI_PORTSC_PRESERVE_BITS) |
        (portsc & XHCI_PORTSC_CHANGE_BITS);

    if (superspeed) {
        if (!(portsc & XHCI_PORTSC_PED)) {
            _write32(op, off, write | XHCI_PORTSC_WPR);
            (void)_wait_bits32(
                (volatile u32 *)(op + off),
                XHCI_PORTSC_WPR,
                false,
                250
            );
            delay_ms(20);
        }
    } else {
        _write32(op, off, write | XHCI_PORTSC_PR);
        delay_ms(60);

        _write32(op, off, write);
        (void)_wait_bits32(
            (volatile u32 *)(op + off),
            XHCI_PORTSC_PR,
            false,
            250
        );
    }

    u64 enable_start = arch_timer_ticks();
    u64 enable_timeout = ms_to_ticks(1000);

    while ((arch_timer_ticks() - enable_start) < enable_timeout) {
        portsc = _read32(op, off);
        if (!(portsc & XHCI_PORTSC_CCS)) {
            break;
        }

        if (portsc & XHCI_PORTSC_PED) {
            break;
        }

        cpu_pause();
    }

    psiv = (portsc >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;
    if (out_speed) {
        *out_speed = _xhci_speed_from_psiv(psiv);
    }

    u32 change = portsc & XHCI_PORTSC_CHANGE_BITS;
    if (change) {
        _xhci_ack_port_change_bits(op, off, portsc);
    }

    bool ok = (portsc & XHCI_PORTSC_CCS) != 0 && (portsc & XHCI_PORTSC_PED) != 0;
    arch_phys_unmap(map, XHCI_MMIO_SIZE);
    return ok;
}


u8 _xhci_speed_to_psiv(usb_speed_t speed) {
    switch (speed) {
    case USB_SPEED_LOW:
        return 2;
    case USB_SPEED_HIGH:
        return 3;
    case USB_SPEED_SUPER:
        return 4;
    case USB_SPEED_FULL:
    default:
        return 1;
    }
}


void _xhci_enable_port_power(xhci_controller_t *ctrl, volatile u8 *op) {
    if (!ctrl || !op) {
        return;
    }

    size_t limit = ctrl->max_ports;
    if (limit > XHCI_MAX_SCAN_PORTS) {
        limit = XHCI_MAX_SCAN_PORTS;
    }

    for (size_t port = 1; port <= limit; port++) {
        size_t off = XHCI_OP_PORTSC_BASE + (port - 1) * XHCI_OP_PORTSC_STRIDE;
        if (ctrl->cap_length + off + sizeof(u32) > XHCI_MMIO_SIZE) {
            break;
        }

        u32 portsc = _read32(op, off);
        if (portsc & XHCI_PORTSC_PP) {
            continue;
        }

        u32 write =
            (portsc & XHCI_PORTSC_PRESERVE_BITS) |
            XHCI_PORTSC_PP |
            (portsc & XHCI_PORTSC_CHANGE_BITS);
        _write32(op, off, write);
    }
}

void _xhci_log_port_snapshot(xhci_controller_t *ctrl, volatile u8 *op) {
    if (!ctrl || !op) {
        return;
    }

    size_t limit = ctrl->max_ports;
    if (limit > XHCI_MAX_SCAN_PORTS) {
        limit = XHCI_MAX_SCAN_PORTS;
    }

    for (size_t port = 1; port <= limit; port++) {
        size_t off = XHCI_OP_PORTSC_BASE + (port - 1) * XHCI_OP_PORTSC_STRIDE;
        if (ctrl->cap_length + off + sizeof(u32) > XHCI_MMIO_SIZE) {
            break;
        }

        u32 portsc = _read32(op, off);

        log_debug(
            "xHCI %u:%u.%u port %zu status=%#x speed=%u",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            port,
            portsc,
            (unsigned)((portsc >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK)
        );
    }
}

size_t _xhci_wait_for_ports(
    xhci_controller_t *ctrl,
    volatile u8 *op,
    u32 timeout_ms
) {
    if (!ctrl || !op) {
        return 0;
    }

    u64 start = arch_timer_ticks();
    u64 timeout = ms_to_ticks(timeout_ms);

    size_t connected = _xhci_scan_ports(ctrl, op, true, false);

    while (!connected && (arch_timer_ticks() - start) < timeout) {
        if (sched_is_running() && sched_current()) {
            sched_yield();
        } else {
            cpu_pause();
        }

        connected = _xhci_scan_ports(ctrl, op, true, false);
    }

    if (connected) {
        (void)_xhci_scan_ports(ctrl, op, true, true);
    }

    return connected;
}
