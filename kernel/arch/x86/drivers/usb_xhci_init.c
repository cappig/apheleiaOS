#include "usb_xhci.h"

#include <inttypes.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include <x86/mm/physical.h>
#include "usb_hcd_common.h"
#include "usb_xhci_internal.h"
#include <x86/apic.h>
#include <x86/asm.h>
#include <x86/irq.h>

xhci_controller_t controllers[XHCI_MAX_DEVICES];
size_t controller_count = 0;

bool msi_handler_registered = false;
bool legacy_handler_registered = false;
u8 legacy_irq_line = 0xff;
sched_thread_t *xhci_watchdog_thread = NULL;
volatile bool xhci_watchdog_stop = false;
static bool xhci_driver_loaded = false;

const driver_desc_t xhci_driver_desc = {
    .name = "xhci",
    .deps = NULL,
    .stage = DRIVER_STAGE_STORAGE,
    .load = xhci_driver_load,
    .unload = xhci_driver_unload,
    .is_busy = xhci_driver_busy,
};

static const char *_xhci_health_name(xhci_health_t state) {
    switch (state) {
    case XHCI_HEALTH_HEALTHY:
        return "healthy";
    case XHCI_HEALTH_DEGRADED:
        return "degraded";
    case XHCI_HEALTH_OFFLINE:
        return "offline";
    default:
        return "unknown";
    }
}

static u16 _xhci_decode_scratchpad_count(u32 hcsp2) {
    u16 lo = (u16)((hcsp2 >> 27) & 0x1fU);
    u16 hi = (u16)((hcsp2 >> 16) & 0x3e0U);
    return (u16)(hi | lo);
}

void _xhci_set_health_state(
    xhci_controller_t *ctrl,
    xhci_health_t state,
    const char *reason
) {
    if (!ctrl || ctrl->health_state == state) {
        return;
    }

    xhci_health_t prev = ctrl->health_state;
    ctrl->health_state = state;

    if (state == XHCI_HEALTH_HEALTHY) {
        log_info(
            "xHCI %u:%u.%u state %s -> %s",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            _xhci_health_name(prev),
            _xhci_health_name(state)
        );
        return;
    }

    if (reason) {
        log_warn(
            "xHCI %u:%u.%u state %s -> %s (%s)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            _xhci_health_name(prev),
            _xhci_health_name(state),
            reason
        );
        return;
    }

    log_warn(
        "xHCI %u:%u.%u state %s -> %s",
        ctrl->bus,
        ctrl->slot,
        ctrl->func,
        _xhci_health_name(prev),
        _xhci_health_name(state)
    );
}

static inline void _xhci_write64_order(
    volatile void *base,
    size_t offset,
    u64 value,
    bool hi_first
) {
    if (!base) {
        return;
    }

    if (hi_first) {
        _write32(base, offset + 4, (u32)(value >> 32));
        _write32(base, offset, (u32)(value & 0xffffffffULL));
        return;
    }

    _write32(base, offset, (u32)(value & 0xffffffffULL));
    _write32(base, offset + 4, (u32)(value >> 32));
}

bool _xhci_write64_checked(
    xhci_controller_t *ctrl,
    volatile void *base,
    size_t offset,
    u64 value,
    u64 verify_mask
) {
    if (!ctrl || !base) {
        return false;
    }

    bool hi_first = ctrl->write64_hi_first;
    bool matched = false;
    u64 actual = 0;

    _xhci_write64_order(base, offset, value, hi_first);

    for (size_t i = 0; i < 256; i++) {
        actual = _read64(base, offset);
        if (((actual ^ value) & verify_mask) == 0) {
            matched = true;
            break;
        }

        cpu_pause();
    }

    // if (!ctrl->write64_order_tested) {
    //     log_debug(
    //         "xHCI %u:%u.%u selected %s 64-bit MMIO write order",
    //         ctrl->bus,
    //         ctrl->slot,
    //         ctrl->func,
    //         hi_first ? "hi->lo" : "lo->hi"
    //     );
    // }

    ctrl->write64_order_tested = true;
    return matched;
}

bool _wait_bits32(const volatile u32 *reg, u32 mask, bool set, u32 timeout_ms) {
    if (!reg) {
        return false;
    }

    u64 start = arch_timer_ticks();
    u64 timeout = ms_to_ticks(timeout_ms);

    for (;;) {
        u32 value = *reg;
        if (((value & mask) != 0) == set) {
            return true;
        }

        if ((arch_timer_ticks() - start) >= timeout) {
            return false;
        }

        if (sched_is_running() && sched_current()) {
            sched_yield();
        } else {
            cpu_pause();
        }
    }
}

void _xhci_op_lock(xhci_controller_t *ctrl) {
    if (!ctrl) {
        return;
    }

    for (;;) {
        if (!__sync_lock_test_and_set(&ctrl->op_lock, 1)) {
            return;
        }

        while (ctrl->op_lock) {
            if (sched_is_running() && sched_current()) {
                sched_yield();
            } else {
                cpu_pause();
            }
        }
    }
}

void _xhci_op_unlock(xhci_controller_t *ctrl) {
    if (!ctrl) {
        return;
    }

    __sync_lock_release(&ctrl->op_lock);
}

bool _xhci_alloc_dma_pages(
    xhci_controller_t *ctrl,
    size_t pages,
    u64 *out_paddr
) {
    if (!ctrl || !pages || !out_paddr) {
        return false;
    }

    u64 paddr = (u64)(uintptr_t)alloc_frames(pages);
    if (!paddr) {
        return false;
    }

    u64 bytes = pages * XHCI_DMA_BYTES;
    if (!ctrl->supports_64bit && paddr + bytes - 1 > 0xffffffffULL) {
        free_frames((void *)(uintptr_t)paddr, pages);
        return false;
    }

    void *map = arch_phys_map(paddr, bytes, 0);
    if (!map) {
        free_frames((void *)(uintptr_t)paddr, pages);
        return false;
    }

    memset(map, 0, bytes);
    arch_phys_unmap(map, bytes);

    *out_paddr = paddr;
    return true;
}

void _xhci_free_dma_pages(u64 *paddr, size_t pages) {
    if (!paddr || !*paddr || !pages) {
        return;
    }

    free_frames((void *)(uintptr_t)*paddr, pages);
    *paddr = 0;
}

void _xhci_reset_wait_state(xhci_controller_t *ctrl) {
    if (!ctrl) {
        return;
    }

    ctrl->cmd_wait_active = false;
    ctrl->cmd_wait_done = false;
    ctrl->cmd_wait_trb = 0;
    ctrl->cmd_wait_cc = 0;
    ctrl->cmd_wait_slot = 0;

    ctrl->xfer_wait_active = false;
    ctrl->xfer_wait_done = false;
    ctrl->xfer_wait_trb = 0;
    ctrl->xfer_wait_slot = 0;
    ctrl->xfer_wait_ep = 0;
    ctrl->xfer_wait_cc = 0;
    ctrl->xfer_wait_residual = 0;
}

bool _xhci_ring_init(
    xhci_controller_t *ctrl,
    xhci_ring_state_t *ring,
    u64 ring_paddr
) {
    if (!ctrl || !ring || !ring_paddr) {
        return false;
    }

    ring->paddr = ring_paddr;
    ring->trbs = (u16)(XHCI_DMA_BYTES / sizeof(xhci_trb_t));
    ring->enqueue = 0;
    ring->cycle = 1;

    if (ring->trbs < 4) {
        return false;
    }

    void *map = arch_phys_map(ring->paddr, XHCI_DMA_BYTES, 0);
    if (!map) {
        return false;
    }

    memset(map, 0, XHCI_DMA_BYTES);

    xhci_trb_t *trbs = map;
    size_t link_idx = ring->trbs - 1;

    trbs[link_idx].parameter_lo = (u32)(ring->paddr & ~0x0fULL);
    trbs[link_idx].parameter_hi = (u32)(ring->paddr >> 32);
    trbs[link_idx].status = 0;
    trbs[link_idx].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
        XHCI_LINK_TOGGLE_CYCLE |
        XHCI_TRB_CYCLE;

    arch_phys_unmap(map, XHCI_DMA_BYTES);
    return true;
}

bool _xhci_ring_enqueue(
    xhci_controller_t *ctrl,
    xhci_ring_state_t *ring,
    const xhci_trb_t *trb,
    u64 *out_trb_paddr
) {
    if (!ctrl || !ring || !trb || !ring->paddr || ring->trbs < 4) {
        return false;
    }

    if (ring->enqueue >= ring->trbs - 1) {
        ring->enqueue = 0;
    }

    void *map = arch_phys_map(ring->paddr, XHCI_DMA_BYTES, 0);
    if (!map) {
        return false;
    }

    xhci_trb_t *entries = map;

    u16 idx = ring->enqueue;
    xhci_trb_t write = *trb;
    write.control &= ~XHCI_TRB_CYCLE;
    if (ring->cycle) {
        write.control |= XHCI_TRB_CYCLE;
    }

    entries[idx] = write;

    if (out_trb_paddr) {
        *out_trb_paddr = ring->paddr + (u64)idx * sizeof(xhci_trb_t);
    }

    idx++;

    if (idx >= ring->trbs - 1) {
        xhci_trb_t *link = &entries[ring->trbs - 1];
        link->control &= ~XHCI_TRB_CYCLE;
        if (ring->cycle) {
            link->control |= XHCI_TRB_CYCLE;
        }

        idx = 0;
        ring->cycle ^= 1U;
    }

    ring->enqueue = idx;

    arch_phys_unmap(map, XHCI_DMA_BYTES);
    return true;
}

bool _xhci_get_mmio_base(const pci_found_t *node, u64 *out_mmio_base) {
    if (!node || !out_mmio_base) {
        return false;
    }

    u32 bar0 = pci_read_config(node->bus, node->slot, node->func, XHCI_BAR0, 4);
    if (!bar0 || bar0 == 0xffffffffU || (bar0 & 1U)) {
        return false;
    }

    u64 mmio = (u64)(bar0 & ~0x0fU);

    if ((bar0 & 0x6U) == 0x4U) {
        u32 bar1 = pci_read_config(
            node->bus,
            node->slot,
            node->func,
            XHCI_BAR0 + 4,
            4
        );
        mmio |= ((u64)bar1 << 32);
    }

    *out_mmio_base = mmio;
    return true;
}

xhci_controller_t *_xhci_find_by_hcd(size_t hcd_id) {
    if (!hcd_id) {
        return NULL;
    }

    for (size_t i = 0; i < ARRAY_LEN(controllers); i++) {
        xhci_controller_t *ctrl = &controllers[i];
        if (ctrl->used && ctrl->hcd_id == hcd_id) {
            return ctrl;
        }
    }

    return NULL;
}

bool _xhci_runtime_available(const xhci_controller_t *ctrl) {
    if (!ctrl || !ctrl->rt_offset) {
        return false;
    }

    size_t rt_end =
        (size_t)ctrl->rt_offset + XHCI_RT_IR0_OFF + XHCI_IR_ERDP_OFF + sizeof(u64);

    return rt_end <= XHCI_MMIO_SIZE;
}

u32 _xhci_read_usbsts(xhci_controller_t *ctrl) {
    if (!ctrl) {
        return 0xffffffffU;
    }

    void *map = arch_phys_map(ctrl->mmio_base, XHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (!map) {
        return 0xffffffffU;
    }

    volatile u8 *op = (volatile u8 *)map + ctrl->cap_length;
    u32 st = _read32(op, XHCI_OP_USBSTS_OFF);
    arch_phys_unmap(map, XHCI_MMIO_SIZE);
    return st;
}

void _xhci_log_fault_snapshot(xhci_controller_t *ctrl, const char *reason) {
    if (!ctrl || ctrl->first_fault_logged) {
        return;
    }

    ctrl->first_fault_logged = true;

    log_warn(
        "xHCI %u:%u.%u fault: %s",
        ctrl->bus,
        ctrl->slot,
        ctrl->func,
        reason ? reason : "unknown"
    );
}

bool _xhci_ensure_running(xhci_controller_t *ctrl) {
    if (!ctrl) {
        return false;
    }

    void *map = arch_phys_map(ctrl->mmio_base, XHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (!map) {
        return false;
    }

    volatile u8 *op = (volatile u8 *)map + ctrl->cap_length;
    u32 status = _read32(op, XHCI_OP_USBSTS_OFF);
    u32 cmd = _read32(op, XHCI_OP_USBCMD_OFF);
    bool runstop_was_cleared = (cmd & XHCI_USBCMD_RUNSTOP) == 0;

    if (status & XHCI_USBSTS_HSE) {
        _xhci_log_fault_snapshot(ctrl, "host system error");
        ctrl->commands_healthy = false;
        _xhci_set_health_state(ctrl, XHCI_HEALTH_DEGRADED, "host system error");
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    if (runstop_was_cleared) {
        _xhci_log_fault_snapshot(ctrl, "RUNSTOP cleared");
        ctrl->commands_healthy = false;
        _xhci_set_health_state(ctrl, XHCI_HEALTH_DEGRADED, "RUNSTOP cleared");
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    if (status & XHCI_USBSTS_CNR) {
        bool cnr_cleared = _wait_bits32(
            (volatile u32 *)(op + XHCI_OP_USBSTS_OFF),
            XHCI_USBSTS_CNR,
            false,
            XHCI_CNR_TIMEOUT_MS
        );
        if (!cnr_cleared) {
            u32 now = _read32(op, XHCI_OP_USBSTS_OFF);
            log_warn(
                "xHCI %u:%u.%u still not ready (usbsts=%#x)",
                ctrl->bus,
                ctrl->slot,
                ctrl->func,
                now
            );

            _xhci_log_fault_snapshot(ctrl, "controller not ready");
            ctrl->commands_healthy = false;
            _xhci_set_health_state(ctrl, XHCI_HEALTH_DEGRADED, "controller not ready");
            arch_phys_unmap(map, XHCI_MMIO_SIZE);

            return false;
        }

        status = _read32(op, XHCI_OP_USBSTS_OFF);
    }

    cmd |= XHCI_USBCMD_RUNSTOP;
    if (ctrl->irq_enabled) {
        cmd |= XHCI_USBCMD_INTE;
    } else {
        cmd &= ~XHCI_USBCMD_INTE;
    }

    if ((status & XHCI_USBSTS_HCH) || runstop_was_cleared) {
        _write32(op, XHCI_OP_USBCMD_OFF, cmd);
    }

    bool resumed = true;
    if ((status & XHCI_USBSTS_HCH) || runstop_was_cleared) {
        resumed = _wait_bits32(
            (volatile u32 *)(op + XHCI_OP_USBSTS_OFF),
            XHCI_USBSTS_HCH,
            false,
            XHCI_HALT_TIMEOUT_MS
        );
    }

    if (!resumed) {
        u32 now = _read32(op, XHCI_OP_USBSTS_OFF);
        log_warn(
            "xHCI %u:%u.%u controller halted (usbsts=%#x)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            now
        );

        _xhci_log_fault_snapshot(ctrl, "controller halted");
        ctrl->commands_healthy = false;
        _xhci_set_health_state(ctrl, XHCI_HEALTH_DEGRADED, "controller halted");
    }

    if (resumed) {
        bool ready_after_resume = _wait_bits32(
            (volatile u32 *)(op + XHCI_OP_USBSTS_OFF),
            XHCI_USBSTS_CNR,
            false,
            XHCI_CNR_TIMEOUT_MS
        );
        if (!ready_after_resume) {
            resumed = false;

            u32 now = _read32(op, XHCI_OP_USBSTS_OFF);
            log_warn(
                "xHCI %u:%u.%u controller never became ready (usbsts=%#x)",
                ctrl->bus,
                ctrl->slot,
                ctrl->func,
                now
            );

            _xhci_log_fault_snapshot(ctrl, "controller never became ready");
            ctrl->commands_healthy = false;
            _xhci_set_health_state(
                ctrl,
                XHCI_HEALTH_DEGRADED,
                "controller never became ready"
            );
        }
    }

    arch_phys_unmap(map, XHCI_MMIO_SIZE);
    return resumed;
}

bool _xhci_update_erdp(xhci_controller_t *ctrl, volatile void *mmio) {
    if (!ctrl || !mmio || !ctrl->event_ring_trbs || !_xhci_runtime_available(ctrl)) {
        return false;
    }

    volatile u8 *ir = (volatile u8 *)mmio + ctrl->rt_offset + XHCI_RT_IR0_OFF;
    u64 erdp =
        ctrl->event_ring_paddr +
        (u64)ctrl->event_dequeue * (u64)sizeof(xhci_trb_t);

    if (_xhci_write64_checked(ctrl, ir, XHCI_IR_ERDP_OFF, erdp | XHCI_ERDP_EHB, ~0x0fULL)) {
        return true;
    }

    _xhci_write64_mmio(ctrl, ir, XHCI_IR_ERDP_OFF, erdp | XHCI_ERDP_EHB);
    __sync_synchronize();

    return false;
}

bool _xhci_dcbaa_set_entry(
    xhci_controller_t *ctrl,
    size_t index,
    u64 value
) {
    if (!ctrl || !ctrl->dcbaa_paddr) {
        return false;
    }

    if (index > ctrl->max_slots) {
        return false;
    }

    void *map = arch_phys_map(ctrl->dcbaa_paddr, XHCI_DMA_BYTES, 0);
    if (!map) {
        return false;
    }

    u64 *dcbaa = map;
    dcbaa[index] = value;

    arch_phys_unmap(map, XHCI_DMA_BYTES);
    return true;
}

bool _xhci_ring_doorbell(xhci_controller_t *ctrl, u8 db_index, u8 target) {
    if (!ctrl) {
        return false;
    }

    void *mmio = arch_phys_map(ctrl->mmio_base, XHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (!mmio) {
        return false;
    }

    size_t off = (size_t)ctrl->db_offset + (size_t)db_index * sizeof(u32);
    if (off + sizeof(u32) > XHCI_MMIO_SIZE) {
        arch_phys_unmap(mmio, XHCI_MMIO_SIZE);
        return false;
    }

    _write32(mmio, off, (u32)target);
    (void)_read32(mmio, off);
    arch_phys_unmap(mmio, XHCI_MMIO_SIZE);

    return true;
}

static void _xhci_release_dma(xhci_controller_t *ctrl) {
    if (!ctrl) {
        return;
    }

    for (size_t p = 1; p <= XHCI_MAX_SCAN_PORTS; p++) {
        xhci_usb_device_t *dev = ctrl->port_devices[p];
        if (dev) {
            _xhci_release_usb_device(ctrl, p, dev);
        }
    }

    for (size_t i = 0; i < ctrl->scratchpad_count && i < XHCI_MAX_SCRATCHPADS; i++) {
        _xhci_free_dma_pages(&ctrl->scratchpad_paddrs[i], 1);
    }

    ctrl->scratchpad_count = 0;
    _xhci_free_dma_pages(
        &ctrl->scratchpad_array_paddr,
        ctrl->scratchpad_array_pages ? ctrl->scratchpad_array_pages : 1
    );
    ctrl->scratchpad_array_pages = 0;

    _xhci_free_dma_pages(&ctrl->dcbaa_paddr, 1);
    _xhci_free_dma_pages(&ctrl->cmd_ring_paddr, 1);
    _xhci_free_dma_pages(&ctrl->event_ring_paddr, 1);
    _xhci_free_dma_pages(&ctrl->erst_paddr, 1);

    ctrl->cmd_ring = (xhci_ring_state_t){0};
    ctrl->event_ring_trbs = 0;
    ctrl->event_dequeue = 0;
    ctrl->event_cycle = 1;
    ctrl->runtime_ready = false;
    ctrl->commands_healthy = false;
    ctrl->first_fault_logged = false;
    ctrl->event_lock = 0;
    ctrl->event_cycle_sync_pending = false;

    _xhci_reset_wait_state(ctrl);
}

static bool _xhci_setup_rings(xhci_controller_t *ctrl) {
    if (!ctrl) {
        return false;
    }

    _xhci_release_dma(ctrl);

    if (
        !_xhci_alloc_dma_pages(ctrl, 1, &ctrl->dcbaa_paddr) ||
        !_xhci_alloc_dma_pages(ctrl, 1, &ctrl->cmd_ring_paddr) ||
        !_xhci_alloc_dma_pages(ctrl, 1, &ctrl->event_ring_paddr) ||
        !_xhci_alloc_dma_pages(ctrl, 1, &ctrl->erst_paddr)
    ) {
        _xhci_release_dma(ctrl);
        return false;
    }

    if (!_xhci_ring_init(ctrl, &ctrl->cmd_ring, ctrl->cmd_ring_paddr)) {
        _xhci_release_dma(ctrl);
        return false;
    }

    ctrl->cmd_ring.cycle = ctrl->crcr_cycle_one ? 1U : 0U;
    void *cmd_map = arch_phys_map(ctrl->cmd_ring.paddr, XHCI_DMA_BYTES, 0);

    if (!cmd_map) {
        _xhci_release_dma(ctrl);
        return false;
    }

    xhci_trb_t *cmd_trbs = cmd_map;
    size_t cmd_link_idx = ctrl->cmd_ring.trbs - 1;
    cmd_trbs[cmd_link_idx].control &= ~XHCI_TRB_CYCLE;

    if (ctrl->cmd_ring.cycle) {
        cmd_trbs[cmd_link_idx].control |= XHCI_TRB_CYCLE;
    }

    arch_phys_unmap(cmd_map, XHCI_DMA_BYTES);

    ctrl->event_ring_trbs = (u16)(XHCI_DMA_BYTES / sizeof(xhci_trb_t));
    ctrl->event_dequeue = 0;
    ctrl->event_cycle = 1;
    ctrl->event_cycle_sync_pending = true;

    void *erst_map = arch_phys_map(ctrl->erst_paddr, XHCI_DMA_BYTES, 0);
    if (!erst_map) {
        _xhci_release_dma(ctrl);
        return false;
    }

    xhci_erst_entry_t *erst = erst_map;
    erst[0].segment_base_lo = (u32)(ctrl->event_ring_paddr & 0xffffffffULL);
    erst[0].segment_base_hi = (u32)(ctrl->event_ring_paddr >> 32);
    erst[0].segment_size = ctrl->event_ring_trbs;
    erst[0].reserved = 0;

    arch_phys_unmap(erst_map, XHCI_DMA_BYTES);

    if (ctrl->scratchpad_count) {
        if (ctrl->scratchpad_count > XHCI_MAX_SCRATCHPADS) {
            log_warn(
                "xHCI %u:%u.%u scratchpads %u exceed max %u",
                ctrl->bus,
                ctrl->slot,
                ctrl->func,
                ctrl->scratchpad_count,
                XHCI_MAX_SCRATCHPADS
            );
            _xhci_release_dma(ctrl);
            return false;
        }

        size_t arr_bytes = (size_t)ctrl->scratchpad_count * sizeof(u64);
        size_t arr_pages = (arr_bytes + XHCI_DMA_BYTES - 1) / XHCI_DMA_BYTES;

        if (!arr_pages) {
            arr_pages = 1;
        }

        ctrl->scratchpad_array_pages = (u16)arr_pages;
        if (
            !_xhci_alloc_dma_pages(
                ctrl,
                ctrl->scratchpad_array_pages,
                &ctrl->scratchpad_array_paddr
            )
        ) {
            _xhci_release_dma(ctrl);
            return false;
        }

        size_t arr_map_bytes = (size_t)ctrl->scratchpad_array_pages * XHCI_DMA_BYTES;
        void *arr_map = arch_phys_map(ctrl->scratchpad_array_paddr, arr_map_bytes, 0);

        if (!arr_map) {
            _xhci_release_dma(ctrl);
            return false;
        }

        u64 *arr = arr_map;
        memset(arr, 0, arr_map_bytes);

        for (size_t i = 0; i < ctrl->scratchpad_count; i++) {
            if (!_xhci_alloc_dma_pages(ctrl, 1, &ctrl->scratchpad_paddrs[i])) {
                arch_phys_unmap(arr_map, arr_map_bytes);
                _xhci_release_dma(ctrl);
                return false;
            }

            arr[i] = ctrl->scratchpad_paddrs[i];
        }

        arch_phys_unmap(arr_map, arr_map_bytes);

        if (!_xhci_dcbaa_set_entry(ctrl, 0, ctrl->scratchpad_array_paddr)) {
            _xhci_release_dma(ctrl);
            return false;
        }
    }

    _xhci_reset_wait_state(ctrl);
    return true;
}

static bool _xhci_validate_runtime_programming(
    xhci_controller_t *ctrl,
    volatile void *mmio,
    volatile u8 *op
) {
    if (!ctrl || !mmio || !op || !_xhci_runtime_available(ctrl)) {
        return false;
    }

    volatile u8 *ir = (volatile u8 *)mmio + ctrl->rt_offset + XHCI_RT_IR0_OFF;

    const u64 crcr_mask = ~0x3fULL;
    const u64 dcbaap_mask = ~0x3fULL;
    const u64 erstba_mask = ~0x3fULL;
    const u64 erdp_mask = ~0x0fULL;

    u64 crcr = _read64(op, XHCI_OP_CRCR_OFF);
    u64 dcbaap = _read64(op, XHCI_OP_DCBAAP_OFF);
    u64 erstba = _read64(ir, XHCI_IR_ERSTBA_OFF);
    u64 erdp = _read64(ir, XHCI_IR_ERDP_OFF);
    u64 expected_erdp =
        ctrl->event_ring_paddr +
        (u64)ctrl->event_dequeue * (u64)sizeof(xhci_trb_t);

    u32 erstsz = _read32(ir, XHCI_IR_ERSTSZ_OFF);

    u64 expected_crcr =
        ctrl->cmd_ring.paddr | (ctrl->crcr_cycle_one ? 1ULL : 0ULL);

    bool crcr_ok = (((crcr ^ expected_crcr) & crcr_mask) == 0);
    bool dcbaap_ok = (((dcbaap ^ ctrl->dcbaa_paddr) & dcbaap_mask) == 0);
    bool erstba_ok = (((erstba ^ ctrl->erst_paddr) & erstba_mask) == 0);
    bool erdp_ok = (((erdp ^ expected_erdp) & erdp_mask) == 0);
    bool erstsz_ok = erstsz == 1U;

    bool crcr_silent = (crcr & crcr_mask) == 0;
    bool erdp_silent = (erdp & erdp_mask) == 0;

    if (!dcbaap_ok || !erstba_ok || !erstsz_ok) {
        log_warn(
            "xHCI %u:%u.%u runtime programming invalid"
            " (dcbaap=%#" PRIx64 " erstba=%#" PRIx64 " erstsz=%u)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            dcbaap,
            erstba,
            erstsz
        );
        return false;
    }

    if (!erdp_ok && !erdp_silent) {
        log_warn(
            "xHCI %u:%u.%u runtime programming invalid"
            " (erdp=%#" PRIx64 " expected=%#" PRIx64 ")",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            erdp,
            expected_erdp
        );
        return false;
    }

    if (!erdp_ok) {
        log_debug(
            "xHCI %u:%u.%u runtime ERDP readback quirk (erdp=%#" PRIx64 ")",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            erdp
        );
    }

    if (!crcr_ok) {
        log_debug(
            "xHCI %u:%u.%u runtime CRCR readback quirk"
            " (crcr=%#" PRIx64 " expected=%#" PRIx64 "%s)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            crcr,
            expected_crcr,
            crcr_silent ? ", silent" : ""
        );
    }

    return true;
}

static bool _xhci_program_runtime(
    xhci_controller_t *ctrl,
    volatile void *mmio,
    volatile u8 *op
) {
    if (!ctrl || !mmio || !op) {
        return false;
    }

    if (
        !ctrl->dcbaa_paddr ||
        !ctrl->cmd_ring.paddr ||
        !ctrl->event_ring_paddr ||
        !ctrl->erst_paddr ||
        !_xhci_runtime_available(ctrl)
    ) {
        return false;
    }

    volatile u8 *ir = (volatile u8 *)mmio + ctrl->rt_offset + XHCI_RT_IR0_OFF;

    if (
        !_xhci_write64_checked(
            ctrl,
            op,
            XHCI_OP_DCBAAP_OFF,
            ctrl->dcbaa_paddr,
            ~0x3fULL
        )
    ) {
        log_warn(
            "xHCI %u:%u.%u DCBAAP readback mismatch",
            ctrl->bus,
            ctrl->slot,
            ctrl->func
        );
        return false;
    }

    u64 crcr_value = ctrl->cmd_ring.paddr | (ctrl->crcr_cycle_one ? 1ULL : 0ULL);
    _xhci_write64_order(
        op,
        XHCI_OP_CRCR_OFF,
        crcr_value,
        ctrl->crcr_hi_first
    );
    __sync_synchronize();

    u32 config = _read32(op, XHCI_OP_CONFIG_OFF);
    config &= ~XHCI_CONFIG_MAX_SLOTS_MASK;
    config |= (u32)ctrl->max_slots;
    _write32(op, XHCI_OP_CONFIG_OFF, config);

    _write32(ir, XHCI_IR_IMOD_OFF, 0);
    _write32(ir, XHCI_IR_ERSTSZ_OFF, 1);

    if (
        !_xhci_write64_checked(
            ctrl,
            ir,
            XHCI_IR_ERSTBA_OFF,
            ctrl->erst_paddr,
            ~0x3fULL
        )
    ) {
        log_warn(
            "xHCI %u:%u.%u ERSTBA readback mismatch",
            ctrl->bus,
            ctrl->slot,
            ctrl->func
        );
        return false;
    }

    ctrl->event_dequeue = 0;
    ctrl->event_cycle = 1;

    if (!_xhci_update_erdp(ctrl, mmio)) {
        log_debug(
            "xHCI %u:%u.%u ERDP readback mismatch during setup",
            ctrl->bus,
            ctrl->slot,
            ctrl->func
        );
    }

    u32 iman = _read32(ir, XHCI_IR_IMAN_OFF);
    iman |= XHCI_IMAN_IP;

    if (ctrl->irq_enabled) {
        iman |= XHCI_IMAN_IE;
    } else {
        iman &= ~XHCI_IMAN_IE;
    }

    _write32(ir, XHCI_IR_IMAN_OFF, iman);

    __sync_synchronize();

    if (!_xhci_validate_runtime_programming(ctrl, mmio, op)) {
        delay_ms(2);
        if (!_xhci_validate_runtime_programming(ctrl, mmio, op)) {
            log_warn(
                "xHCI %u:%u.%u runtime programming check failed",
                ctrl->bus,
                ctrl->slot,
                ctrl->func
            );
            ctrl->runtime_ready = false;
            return false;
        }
    }

    ctrl->runtime_ready = true;
    return true;
}

static void _xhci_try_legacy_handoff(volatile void *mmio) {
    if (!mmio) {
        return;
    }

    u32 hccparams1 = _read32(mmio, XHCI_HCCPARAMS1_OFF);
    u16 xecp = (u16)((hccparams1 >> 16) & 0xffffU);
    if (!xecp) {
        return;
    }

    size_t ext_offset = (size_t)xecp * 4;

    for (size_t guard = 0; guard < 64 && ext_offset + 4 < XHCI_MMIO_SIZE; guard++) {
        u32 cap = _read32(mmio, ext_offset);
        u8 cap_id = (u8)(cap & 0xffU);
        u8 next = (u8)((cap >> 8) & 0xffU);

        if (cap_id == 1) {
            u32 legsup = cap | (1U << 24);
            _write32(mmio, ext_offset, legsup);

            u64 start = arch_timer_ticks();
            u64 timeout = ms_to_ticks(1500);

            while ((arch_timer_ticks() - start) < timeout) {
                u32 now = _read32(mmio, ext_offset);

                bool bios_owned = (now & (1U << 16)) != 0;
                bool os_owned = (now & (1U << 24)) != 0;

                if (!bios_owned && os_owned) {
                    break;
                }

                cpu_pause();
            }

            if (ext_offset + 8 < XHCI_MMIO_SIZE) {
                _write32(mmio, ext_offset + 4, 0);
            }

            return;
        }

        if (!next) {
            break;
        }

        ext_offset += (size_t)next * 4;
    }
}

static bool _xhci_reset_and_configure(xhci_controller_t *ctrl, bool scan_ports) {
    if (!ctrl) {
        return false;
    }

    void *map = arch_phys_map(ctrl->mmio_base, XHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (!map) {
        return false;
    }

    volatile u8 *mmio = map;
    ctrl->runtime_ready = false;
    ctrl->commands_healthy = false;
    ctrl->first_fault_logged = false;

    ctrl->cap_length = (u8)(_read32(mmio, XHCI_CAPLENGTH_OFF) & 0xffU);
    if (!ctrl->cap_length) {
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    u32 hcsp1 = _read32(mmio, XHCI_HCSPARAMS1_OFF);
    u32 hcsp2 = _read32(mmio, XHCI_HCSPARAMS2_OFF);

    ctrl->max_slots = (u8)(hcsp1 & 0xffU);
    if (!ctrl->max_slots) {
        ctrl->max_slots = 1;
    }

    ctrl->max_ports = (u8)((hcsp1 >> 24) & 0xffU);
    if (!ctrl->max_ports) {
        ctrl->max_ports = 1;
    }

    ctrl->scratchpad_count = _xhci_decode_scratchpad_count(hcsp2);

    log_debug(
        "xHCI %u:%u.%u caps slots=%u ports=%u spad=%u hcsp2=%#x",
        ctrl->bus,
        ctrl->slot,
        ctrl->func,
        ctrl->max_slots,
        ctrl->max_ports,
        ctrl->scratchpad_count,
        hcsp2
    );

    u32 hccparams1 = _read32(mmio, XHCI_HCCPARAMS1_OFF);
    ctrl->supports_64bit = (hccparams1 & 1U) != 0;
    ctrl->context_64 = (hccparams1 & (1U << 2)) != 0;

    ctrl->db_offset = _read32(mmio, XHCI_DBOFF_OFF) & ~0x3U;
    ctrl->rt_offset = _read32(mmio, XHCI_RTSOFF_OFF) & ~0x1fU;

    usb_hcd_info_t hcd_info = {
        .kind = USB_HCD_XHCI,
        .pci_bus = ctrl->bus,
        .pci_slot = ctrl->slot,
        .pci_func = ctrl->func,
        .vendor_id = ctrl->vendor_id,
        .device_id = ctrl->device_id,
        .max_ports = ctrl->max_ports,
        .irq_driven = ctrl->irq_enabled,
        .msi_enabled = ctrl->msi_enabled,
    };

    _xhci_try_legacy_handoff(mmio);

    volatile u8 *op = mmio + ctrl->cap_length;

    u32 pagesize = _read32(op, XHCI_OP_PAGESIZE_OFF);
    if (!(pagesize & 1U)) {
        log_warn(
            "xHCI %u:%u.%u unsupported page size bitmap %#x",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            pagesize
        );
    }

    u32 cmd = _read32(op, XHCI_OP_USBCMD_OFF);
    cmd &= ~XHCI_USBCMD_RUNSTOP;
    _write32(op, XHCI_OP_USBCMD_OFF, cmd);

    bool halted_before_reset = _wait_bits32(
        (volatile u32 *)(op + XHCI_OP_USBSTS_OFF),
        XHCI_USBSTS_HCH,
        true,
        XHCI_HALT_TIMEOUT_MS
    );

    if (!halted_before_reset) {
        log_warn(
            "xHCI %u:%u.%u failed to halt controller before reset",
            ctrl->bus,
            ctrl->slot,
            ctrl->func
        );
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    cmd = _read32(op, XHCI_OP_USBCMD_OFF);
    _write32(op, XHCI_OP_USBCMD_OFF, cmd | XHCI_USBCMD_HCRST);

    bool reset_done = _wait_bits32(
        (volatile u32 *)(op + XHCI_OP_USBCMD_OFF),
        XHCI_USBCMD_HCRST,
        false,
        1000
    );

    if (!reset_done) {
        log_warn("xHCI %u:%u.%u reset timed out", ctrl->bus, ctrl->slot, ctrl->func);
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    bool ready_after_reset = _wait_bits32(
        (volatile u32 *)(op + XHCI_OP_USBSTS_OFF),
        XHCI_USBSTS_CNR,
        false,
        XHCI_CNR_TIMEOUT_MS
    );

    if (!ready_after_reset) {
        u32 now = _read32(op, XHCI_OP_USBSTS_OFF);
        log_warn(
            "xHCI %u:%u.%u controller not ready after reset (usbsts=%#x)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            now
        );
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    delay_ms(100);

    u32 status = _read32(op, XHCI_OP_USBSTS_OFF);
    u32 ack = status & (XHCI_USBSTS_EINT | XHCI_USBSTS_PCD | XHCI_USBSTS_HSE);
    if (ack) {
        _write32(op, XHCI_OP_USBSTS_OFF, ack);
    }

    if (!_xhci_setup_rings(ctrl)) {
        log_warn("xHCI %u:%u.%u ring setup failed", ctrl->bus, ctrl->slot, ctrl->func);
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    if (!_xhci_program_runtime(ctrl, mmio, op)) {
        log_warn("xHCI %u:%u.%u runtime programming failed", ctrl->bus, ctrl->slot, ctrl->func);
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    cmd = _read32(op, XHCI_OP_USBCMD_OFF);
    if (ctrl->irq_enabled) {
        cmd |= XHCI_USBCMD_INTE;
    } else {
        cmd &= ~XHCI_USBCMD_INTE;
    }

    cmd |= XHCI_USBCMD_RUNSTOP;
    _write32(op, XHCI_OP_USBCMD_OFF, cmd);

    bool started = _wait_bits32(
        (volatile u32 *)(op + XHCI_OP_USBSTS_OFF),
        XHCI_USBSTS_HCH,
        false,
        XHCI_HALT_TIMEOUT_MS
    );

    if (!started) {
        u32 now = _read32(op, XHCI_OP_USBSTS_OFF);
        log_warn(
            "xHCI %u:%u.%u failed to start controller (usbsts=%#x)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            now
        );
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    bool ready_after_start = _wait_bits32(
        (volatile u32 *)(op + XHCI_OP_USBSTS_OFF),
        XHCI_USBSTS_CNR,
        false,
        XHCI_CNR_TIMEOUT_MS
    );

    if (!ready_after_start) {
        u32 now = _read32(op, XHCI_OP_USBSTS_OFF);
        log_warn(
            "xHCI %u:%u.%u still not ready after start (usbsts=%#x)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            now
        );
        arch_phys_unmap(map, XHCI_MMIO_SIZE);
        return false;
    }

    // Real hardware can raise transient faults if command traffic starts
    // immediately after RUN... give it some rest here
    delay_ms(100);

    ctrl->commands_healthy = true;

    if (!ctrl->hcd_id) {
        if (!usb_register_hcd(&hcd_info, &xhci_hcd_ops, &ctrl->hcd_id)) {
            arch_phys_unmap(map, XHCI_MMIO_SIZE);
            log_warn(
                "xHCI %u:%u.%u failed to register with USB core",
                ctrl->bus,
                ctrl->slot,
                ctrl->func
            );
            return false;
        }
    }

    _xhci_enable_port_power(ctrl, op);

    size_t connected_ports = 0;
    if (scan_ports) {
        connected_ports = _xhci_scan_ports(ctrl, op, true, true);
        if (!connected_ports) {
            connected_ports = _xhci_wait_for_ports(ctrl, op, 1500);
        }

        if (!connected_ports) {
            _xhci_log_port_snapshot(ctrl, op);
        }
    }

    arch_phys_unmap(map, XHCI_MMIO_SIZE);

    if (scan_ports) {
        log_info(
            "xHCI %u:%u.%u initialized (%u ports, %zu connected%s%s)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            ctrl->max_ports,
            connected_ports,
            ctrl->irq_enabled ? ", irq" : "",
            ctrl->runtime_ready ? ", events" : ""
        );
    } else {
        log_info(
            "xHCI %u:%u.%u initialized (%u ports%s%s)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            ctrl->max_ports,
            ctrl->irq_enabled ? ", irq" : "",
            ctrl->runtime_ready ? ", events" : ""
        );
    }

    return true;
}

static void _xhci_msi_irq(UNUSED int_state_t *state) {
    _xhci_service_interrupts();
    lapic_end_int();
}

static void _xhci_legacy_irq(UNUSED int_state_t *state) {
    _xhci_service_interrupts();

    if (usb_hcd_irq_line_supported(legacy_irq_line)) {
        irq_ack(legacy_irq_line);
    }
}

static void _xhci_enable_legacy_irq(xhci_controller_t *ctrl) {
    if (!ctrl) {
        return;
    }

    ctrl->msi_enabled = false;
    ctrl->irq_enabled = false;

    if (!usb_hcd_irq_line_supported(ctrl->irq_line)) {
        log_warn(
            "xHCI %u:%u.%u unsupported IRQ line %u; IRQs disabled",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            ctrl->irq_line
        );
        return;
    }

    if (!legacy_handler_registered) {
        legacy_irq_line = ctrl->irq_line;
        irq_register(legacy_irq_line, _xhci_legacy_irq);
        legacy_handler_registered = true;
    }

    if (ctrl->irq_line != legacy_irq_line) {
        log_warn(
            "xHCI %u:%u.%u IRQ line %u != primary %u; IRQ disabled",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            ctrl->irq_line,
            legacy_irq_line
        );
        return;
    }

    ctrl->irq_enabled = true;
}

static void _xhci_register_irq(xhci_controller_t *ctrl) {
    if (!ctrl) {
        return;
    }

    bool msi = pci_enable_msi(
        ctrl->bus,
        ctrl->slot,
        ctrl->func,
        XHCI_MSI_VECTOR,
        lapic_id()
    );

    if (msi) {
        if (!msi_handler_registered) {
            set_int_handler(XHCI_MSI_VECTOR, _xhci_msi_irq);
            msi_handler_registered = true;
        }

        ctrl->msi_enabled = true;
        ctrl->irq_enabled = true;
        return;
    }

    _xhci_enable_legacy_irq(ctrl);
}

static void _xhci_watchdog_scan_ports(xhci_controller_t *ctrl) {
    if (!ctrl || !ctrl->used || !ctrl->cap_length) {
        return;
    }

    void *map = arch_phys_map(ctrl->mmio_base, XHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (!map) {
        return;
    }

    volatile u8 *op = (volatile u8 *)map + ctrl->cap_length;
    (void)_xhci_scan_ports(ctrl, op, true, false);
    arch_phys_unmap(map, XHCI_MMIO_SIZE);
}

static void _xhci_watchdog_worker(void *arg) {
    (void)arg;

    for (;;) {
        if (xhci_watchdog_stop) {
            break;
        }

        for (size_t i = 0; i < controller_count; i++) {
            xhci_controller_t *ctrl = &controllers[i];
            if (!ctrl->used || !ctrl->cap_length) {
                continue;
            }

            if (!ctrl->commands_healthy || ctrl->health_state != XHCI_HEALTH_HEALTHY) {
                if (ctrl->watchdog_stall_ticks < 0xffffffffU) {
                    ctrl->watchdog_stall_ticks++;
                }

                if (ctrl->watchdog_stall_ticks >= 5) {
                    ctrl->watchdog_stall_ticks = 0;
                    if (ctrl->runtime_ready) {
                        (void)_xhci_poll_events(ctrl, true, true);
                    }
                    _xhci_watchdog_scan_ports(ctrl);
                }
                continue;
            }

            u64 seq = ctrl->irq_seq;
            if (seq == ctrl->watchdog_last_irq_seq) {
                if (ctrl->watchdog_stall_ticks < 0xffffffffU) {
                    ctrl->watchdog_stall_ticks++;
                }
            } else {
                ctrl->watchdog_stall_ticks = 0;
            }

            ctrl->watchdog_last_irq_seq = seq;

            // keep interrupt-driven operation as default; force-poll only when
            // the event path appears stalled for multiple watchdog intervals
            if (ctrl->watchdog_stall_ticks >= 6) {
                ctrl->watchdog_stall_ticks = 0;
                (void)_xhci_poll_events(ctrl, true, true);
            }
        }

        sched_sleep(10);
    }

    xhci_watchdog_thread = NULL;
    xhci_watchdog_stop = false;
    sched_exit();
}

static void _xhci_start_watchdog(void) {
    if (xhci_watchdog_thread) {
        return;
    }

    xhci_watchdog_stop = false;
    xhci_watchdog_thread =
        sched_create_kernel_thread("xhci-watchdog", _xhci_watchdog_worker, NULL);

    if (!xhci_watchdog_thread) {
        log_warn("xHCI failed to create watchdog thread");
        return;
    }

    sched_make_runnable(xhci_watchdog_thread);
}

static void _xhci_stop_watchdog(void) {
    if (!xhci_watchdog_thread) {
        return;
    }

    xhci_watchdog_stop = true;
    sched_unblock_thread(xhci_watchdog_thread);

    if (sched_is_running()) {
        for (size_t i = 0; i < 200 && xhci_watchdog_thread; i++) {
            sched_yield();
        }
    }

    if (xhci_watchdog_thread) {
        xhci_watchdog_thread = NULL;
    }
}

static void _xhci_shutdown_controller(xhci_controller_t *ctrl) {
    if (!ctrl || !ctrl->used) {
        return;
    }

    if (ctrl->hcd_id) {
        if (!usb_unregister_hcd(ctrl->hcd_id)) {
            log_warn(
                "xHCI %u:%u.%u failed to unregister HCD id=%zu",
                ctrl->bus,
                ctrl->slot,
                ctrl->func,
                ctrl->hcd_id
            );
        }
        ctrl->hcd_id = 0;
    }

    void *mmio = arch_phys_map(ctrl->mmio_base, XHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (mmio) {
        volatile u8 *op = (volatile u8 *)mmio + ctrl->cap_length;
        u32 cmd = _read32(op, XHCI_OP_USBCMD_OFF);
        cmd &= ~(XHCI_USBCMD_RUNSTOP | XHCI_USBCMD_INTE);
        _write32(op, XHCI_OP_USBCMD_OFF, cmd);
        (void)_wait_bits32(
            (volatile u32 *)(op + XHCI_OP_USBSTS_OFF),
            XHCI_USBSTS_HCH,
            true,
            XHCI_HALT_TIMEOUT_MS
        );
        arch_phys_unmap(mmio, XHCI_MMIO_SIZE);
    }

    if (ctrl->msi_enabled) {
        (void)pci_disable_msi(ctrl->bus, ctrl->slot, ctrl->func);
    } else if (ctrl->irq_enabled && usb_hcd_irq_line_supported(ctrl->irq_line)) {
        irq_unregister(ctrl->irq_line);
    }

    _xhci_release_dma(ctrl);
    memset(ctrl, 0, sizeof(*ctrl));
}

static bool usb_xhci_init(void) {
    bool found = false;
    pci_found_t *cursor = NULL;

    for (;;) {
        pci_found_t *node = pci_find_node(PCI_SERIAL_BUS, USB_SUBCLASS, cursor);
        if (!node) {
            break;
        }

        cursor = node;

        if (node->header.prog_if != USB_PROGIF_XHCI) {
            continue;
        }

        if (controller_count >= ARRAY_LEN(controllers)) {
            log_warn(
                "xHCI controller table full, skipping %u:%u.%u",
                node->bus,
                node->slot,
                node->func
            );
            continue;
        }

        u64 mmio_base = 0;
        if (!_xhci_get_mmio_base(node, &mmio_base)) {
            log_warn(
                "xHCI %u:%u.%u has no usable MMIO BAR",
                node->bus,
                node->slot,
                node->func
            );
            continue;
        }

        pci_enable_bus_mastering(node->bus, node->slot, node->func);

        xhci_controller_t *ctrl = &controllers[controller_count];
        memset(ctrl, 0, sizeof(*ctrl));

        ctrl->used = true;
        ctrl->bus = node->bus;
        ctrl->slot = node->slot;
        ctrl->func = node->func;
        ctrl->vendor_id = node->header.vendor_id;
        ctrl->device_id = node->header.device_id;
        ctrl->irq_line = (u8)pci_read_config(
            node->bus,
            node->slot,
            node->func,
            PCI_CFG_INT_LINE,
            1
        );
        ctrl->mmio_base = mmio_base;
        ctrl->write64_hi_first = false;
        ctrl->crcr_hi_first = false;
        ctrl->crcr_cycle_one = true;

        _xhci_register_irq(ctrl);

        if (!_xhci_reset_and_configure(ctrl, true)) {
            _xhci_release_dma(ctrl);
            ctrl->used = false;
            continue;
        }

        controller_count++;
        found = true;
    }

    if (found) {
        _xhci_start_watchdog();
    }

    return found;
}

bool xhci_driver_busy(void) {
    return false;
}

driver_err_t xhci_driver_load(void) {
    if (xhci_driver_loaded) {
        return DRIVER_OK;
    }

    if (!usb_core_is_ready()) {
        return DRIVER_ERR_DEPENDENCY;
    }

    if (!usb_xhci_init()) {
        return DRIVER_ERR_INIT_FAILED;
    }

    xhci_driver_loaded = true;
    return DRIVER_OK;
}

driver_err_t xhci_driver_unload(void) {
    if (!xhci_driver_loaded) {
        return DRIVER_OK;
    }

    _xhci_stop_watchdog();

    for (size_t i = 0; i < controller_count; i++) {
        _xhci_shutdown_controller(&controllers[i]);
    }

    controller_count = 0;

    if (legacy_handler_registered && usb_hcd_irq_line_supported(legacy_irq_line)) {
        irq_unregister(legacy_irq_line);
    }

    if (msi_handler_registered) {
        reset_int_handler(XHCI_MSI_VECTOR);
    }

    msi_handler_registered = false;
    legacy_handler_registered = false;
    legacy_irq_line = 0xff;
    xhci_driver_loaded = false;
    return DRIVER_OK;
}
