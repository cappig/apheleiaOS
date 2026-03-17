#include "usb_xhci_internal.h"

#include <inttypes.h>
#include <log/log.h>
#include <string.h>

#include <sys/time.h>
#include "usb_hcd_common.h"

static const char *_xhci_cmd_type_name(u32 type) {
    switch (type) {
    case XHCI_TRB_TYPE_ENABLE_SLOT:
        return "ENABLE_SLOT";
    case XHCI_TRB_TYPE_DISABLE_SLOT:
        return "DISABLE_SLOT";
    case XHCI_TRB_TYPE_ADDRESS_DEVICE:
        return "ADDRESS_DEVICE";
    case XHCI_TRB_TYPE_CONFIGURE_ENDPOINT:
        return "CONFIGURE_ENDPOINT";
    case XHCI_TRB_TYPE_EVALUATE_CONTEXT:
        return "EVALUATE_CONTEXT";
    case XHCI_TRB_TYPE_NOOP_CMD:
        return "NOOP";
    default:
        return "CMD";
    }
}

typedef enum {
    XHCI_WAIT_CMD,
    XHCI_WAIT_XFER,
} xhci_wait_kind_t;

typedef struct {
    bool done;
    u8 cc;
    u8 slot;
    u32 residual;
} xhci_wait_state_t;

static void _xhci_read_wait_state(
    xhci_controller_t *ctrl,
    xhci_wait_kind_t kind,
    xhci_wait_state_t *out_state
) {
    if (!ctrl || !out_state) {
        return;
    }

    unsigned long flags = arch_irq_save();
    if (kind == XHCI_WAIT_CMD) {
        out_state->done = ctrl->cmd_wait_done;
        out_state->cc = ctrl->cmd_wait_cc;
        out_state->slot = ctrl->cmd_wait_slot;
        out_state->residual = 0;
    } else {
        out_state->done = ctrl->xfer_wait_done;
        out_state->cc = ctrl->xfer_wait_cc;
        out_state->slot = 0;
        out_state->residual = ctrl->xfer_wait_residual;
    }
    arch_irq_restore(flags);
}

static void _xhci_copy_wait_outputs(
    xhci_wait_kind_t kind,
    const xhci_wait_state_t *state,
    u8 *out_cc,
    u8 *out_slot,
    u32 *out_residual
) {
    if (!state) {
        return;
    }

    if (out_cc) {
        *out_cc = state->cc;
    }

    if (kind == XHCI_WAIT_CMD) {
        if (out_slot) {
            *out_slot = state->slot;
        }
        return;
    }

    if (out_residual) {
        *out_residual = state->residual;
    }
}

static bool _xhci_wait_common(
    xhci_controller_t *ctrl,
    xhci_wait_kind_t kind,
    u32 timeout_ms,
    u8 *out_cc,
    u8 *out_slot,
    u32 *out_residual
) {
    if (!ctrl) {
        return false;
    }

    u64 start = arch_timer_ticks();
    u32 default_timeout_ms =
        kind == XHCI_WAIT_CMD ? XHCI_CMD_TIMEOUT_MS : XHCI_XFER_TIMEOUT_MS;
    u64 timeout = ms_to_ticks(timeout_ms ? timeout_ms : default_timeout_ms);

    for (;;) {
        xhci_wait_state_t state = {0};
        _xhci_read_wait_state(ctrl, kind, &state);
        if (state.done) {
            _xhci_copy_wait_outputs(
                kind,
                &state,
                out_cc,
                out_slot,
                out_residual
            );
            return true;
        }

        bool timed_out = (arch_timer_ticks() - start) >= timeout;
        if (timed_out) {
            // one final event poll at deadline with cycle resync enabled
            ctrl->event_cycle_sync_pending = true;
        }

        (void)_xhci_poll_events(ctrl, false, true);

        if (timed_out) {
            _xhci_read_wait_state(ctrl, kind, &state);
            if (state.done) {
                _xhci_copy_wait_outputs(
                    kind,
                    &state,
                    out_cc,
                    out_slot,
                    out_residual
                );
                return true;
            }

            return false;
        }

        u32 st = _xhci_read_usbsts(ctrl);
        if (
            st != 0xffffffffU &&
            (st & (XHCI_USBSTS_HCH | XHCI_USBSTS_HSE | XHCI_USBSTS_CNR))
        ) {
            const char *reason = kind == XHCI_WAIT_CMD
                ? "fatal status while waiting for command"
                : "fatal status while waiting for transfer";

            _xhci_log_fault_snapshot(ctrl, reason);
            ctrl->commands_healthy = false;
            _xhci_set_health_state(ctrl, XHCI_HEALTH_DEGRADED, reason);

            return false;
        }

        if (sched_is_running() && sched_current()) {
            sched_yield();
        } else {
            cpu_pause();
        }
    }
}

bool _xhci_wait_cmd(
    xhci_controller_t *ctrl,
    u32 timeout_ms,
    u8 *out_cc,
    u8 *out_slot
) {
    return _xhci_wait_common(
        ctrl,
        XHCI_WAIT_CMD,
        timeout_ms,
        out_cc,
        out_slot,
        NULL
    );
}

bool _xhci_wait_xfer(
    xhci_controller_t *ctrl,
    u32 timeout_ms,
    u8 *out_cc,
    u32 *out_residual
) {
    return _xhci_wait_common(
        ctrl,
        XHCI_WAIT_XFER,
        timeout_ms,
        out_cc,
        NULL,
        out_residual
    );
}

bool _xhci_submit_command(
    xhci_controller_t *ctrl,
    const xhci_trb_t *cmd,
    u8 *out_slot,
    u32 timeout_ms
) {
    if (!ctrl || !cmd) {
        return false;
    }

    u32 cmd_type = _trb_type(cmd);
    const char *cmd_name = _xhci_cmd_type_name(cmd_type);

    if (!ctrl->runtime_ready) {
        log_warn(
            "xHCI %u:%u.%u command %s rejected; runtime not ready",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            cmd_name
        );
        return false;
    }

    if (!_xhci_ensure_running(ctrl)) {
        log_warn(
            "xHCI %u:%u.%u command %s rejected; controller not running",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            cmd_name
        );
        return false;
    }

    _xhci_op_lock(ctrl);

    u64 trb_paddr = 0;
    if (!_xhci_ring_enqueue(ctrl, &ctrl->cmd_ring, cmd, &trb_paddr)) {
        log_warn(
            "xHCI %u:%u.%u command %s enqueue failed (ring=%#" PRIx64 " enq=%u)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            cmd_name,
            ctrl->cmd_ring.paddr,
            ctrl->cmd_ring.enqueue
        );
        _xhci_op_unlock(ctrl);
        return false;
    }

    unsigned long flags = arch_irq_save();
    ctrl->cmd_wait_active = true;
    ctrl->cmd_wait_done = false;
    ctrl->cmd_wait_trb = trb_paddr;
    ctrl->cmd_wait_cc = 0;
    ctrl->cmd_wait_slot = 0;
    arch_irq_restore(flags);

    __sync_synchronize();

    bool doorbell = _xhci_ring_doorbell(ctrl, 0, 0);

    u8 cc = 0xff;
    u8 slot = 0;
    bool waited = doorbell && _xhci_wait_cmd(ctrl, timeout_ms, &cc, &slot);

    flags = arch_irq_save();
    ctrl->cmd_wait_active = false;
    arch_irq_restore(flags);

    _xhci_op_unlock(ctrl);

    if (!doorbell) {
        log_warn(
            "xHCI %u:%u.%u command %s doorbell failed",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            cmd_name
        );

        _xhci_set_health_state(ctrl, XHCI_HEALTH_DEGRADED, "command doorbell failed");
        ctrl->commands_healthy = false;

        return false;
    }

    if (!waited) {
        void *map = arch_phys_map(ctrl->mmio_base, XHCI_MMIO_SIZE, PHYS_MAP_MMIO);
        u32 usbsts = 0xffffffffU;

        if (map) {
            volatile u8 *op = (volatile u8 *)map + ctrl->cap_length;
            usbsts = _read32(op, XHCI_OP_USBSTS_OFF);
            arch_phys_unmap(map, XHCI_MMIO_SIZE);
        }

        log_warn(
            "xHCI %u:%u.%u command %s timed out (usbsts=%#x trb=%#" PRIx64
            " evt=%u/%u)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            cmd_name,
            (unsigned int)usbsts,
            trb_paddr,
            (unsigned int)ctrl->event_dequeue,
            (unsigned int)ctrl->event_cycle
        );

        _xhci_log_fault_snapshot(ctrl, "command timeout");
        _xhci_set_health_state(ctrl, XHCI_HEALTH_DEGRADED, "command timeout");
        ctrl->commands_healthy = false;

        return false;
    }

    if (out_slot) {
        *out_slot = slot;
    }

    if (cc != XHCI_CC_SUCCESS) {
        log_warn(
            "xHCI %u:%u.%u command %s failed (cc=%#x)",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            cmd_name,
            cc
        );

        _xhci_set_health_state(ctrl, XHCI_HEALTH_DEGRADED, "command completion error");
        ctrl->commands_healthy = false;

        return false;
    }

    ctrl->commands_healthy = true;
    _xhci_set_health_state(ctrl, XHCI_HEALTH_HEALTHY, NULL);

    return true;
}

bool _xhci_submit_transfer(
    xhci_controller_t *ctrl,
    xhci_usb_device_t *dev,
    xhci_ring_state_t *ring,
    u8 endpoint_id,
    const xhci_trb_t *trbs,
    size_t trb_count,
    size_t completion_trb_index,
    size_t expected_data_len,
    bool allow_short,
    u32 timeout_ms,
    size_t *out_actual
) {
    if (
        !ctrl ||
        !dev ||
        !ring ||
        !endpoint_id ||
        !trbs ||
        !trb_count ||
        !ctrl->runtime_ready
    ) {
        return false;
    }

    if (!_xhci_ensure_running(ctrl)) {
        return false;
    }

    _xhci_op_lock(ctrl);

    u64 last_trb = 0;
    u64 wait_trb = 0;

    for (size_t i = 0; i < trb_count; i++) {
        if (!_xhci_ring_enqueue(ctrl, ring, &trbs[i], &last_trb)) {
            _xhci_op_unlock(ctrl);
            return false;
        }

        if (i == completion_trb_index) {
            wait_trb = last_trb;
        }
    }

    if (!wait_trb) {
        wait_trb = last_trb;
    }

    unsigned long flags = arch_irq_save();
    ctrl->xfer_wait_active = true;
    ctrl->xfer_wait_done = false;
    ctrl->xfer_wait_trb = wait_trb;
    ctrl->xfer_wait_slot = dev->slot_id;
    ctrl->xfer_wait_ep = endpoint_id;
    ctrl->xfer_wait_cc = 0;
    ctrl->xfer_wait_residual = 0;
    arch_irq_restore(flags);

    __sync_synchronize();

    bool doorbell = _xhci_ring_doorbell(ctrl, dev->slot_id, endpoint_id);

    u8 cc = 0xff;
    u32 residual = 0;
    bool waited = doorbell && _xhci_wait_xfer(ctrl, timeout_ms, &cc, &residual);

    flags = arch_irq_save();
    ctrl->xfer_wait_active = false;
    arch_irq_restore(flags);

    _xhci_op_unlock(ctrl);

    if (!waited) {
        return false;
    }

    if (cc != XHCI_CC_SUCCESS && (!allow_short || cc != XHCI_CC_SHORT_PACKET)) {
        log_warn(
            "xHCI %u:%u.%u transfer failed slot=%u ep=%u cc=%#x",
            ctrl->bus,
            ctrl->slot,
            ctrl->func,
            dev->slot_id,
            endpoint_id,
            cc
        );
        return false;
    }

    size_t actual = expected_data_len;

    if (residual <= expected_data_len) {
        actual = expected_data_len - residual;
    }

    if (out_actual) {
        *out_actual = actual;
    }

    return true;
}

void _xhci_set_slot_ctx(
    xhci_controller_t *ctrl,
    u32 *slot_ctx,
    u8 speed_id,
    u8 context_entries,
    u8 root_port
) {
    (void)ctrl;

    if (!slot_ctx) {
        return;
    }

    slot_ctx[0] =
        ((u32)speed_id << 20) |
        ((u32)context_entries << 27);

    slot_ctx[1] = (u32)root_port << 16;
}

void _xhci_set_ep_ctx(
    u32 *ep_ctx,
    u8 ep_type,
    u16 max_packet,
    u64 dequeue_paddr,
    u16 avg_trb_len
) {
    if (!ep_ctx) {
        return;
    }

    ep_ctx[0] = 0;
    ep_ctx[1] =
        (3U << 1) |
        ((u32)ep_type << 3) |
        ((u32)max_packet << 16);
    ep_ctx[2] = (u32)(dequeue_paddr & ~0x0fULL) | 1U;
    ep_ctx[3] = (u32)(dequeue_paddr >> 32);
    ep_ctx[4] = (u32)avg_trb_len;
    ep_ctx[5] = 0;
}

bool _xhci_prepare_address_ctx(xhci_usb_device_t *dev) {
    if (!dev || !dev->ctrl || !dev->input_ctx_paddr || !dev->ep0_ring.paddr) {
        return false;
    }

    xhci_controller_t *ctrl = dev->ctrl;
    size_t ctx = _ctx_bytes(ctrl);

    void *map = arch_phys_map(dev->input_ctx_paddr, XHCI_DMA_BYTES, 0);
    if (!map) {
        return false;
    }

    memset(map, 0, XHCI_DMA_BYTES);

    u32 *icc = (u32 *)((u8 *)map + 0 * ctx);
    u32 *slot = (u32 *)((u8 *)map + 1 * ctx);
    u32 *ep0 = (u32 *)((u8 *)map + 2 * ctx);

    icc[0] = 0;
    icc[1] = (1U << 0) | (1U << 1);

    _xhci_set_slot_ctx(ctrl, slot, dev->port_speed_id, 1, (u8)dev->port);
    _xhci_set_ep_ctx(ep0, XHCI_EP_TYPE_CTRL, dev->max_packet0, dev->ep0_ring.paddr, 8);

    arch_phys_unmap(map, XHCI_DMA_BYTES);
    return true;
}

bool _xhci_prepare_ep0_eval_ctx(xhci_usb_device_t *dev) {
    if (!dev || !dev->ctrl || !dev->input_ctx_paddr || !dev->ep0_ring.paddr) {
        return false;
    }

    xhci_controller_t *ctrl = dev->ctrl;
    size_t ctx = _ctx_bytes(ctrl);

    void *map = arch_phys_map(dev->input_ctx_paddr, XHCI_DMA_BYTES, 0);
    if (!map) {
        return false;
    }

    memset(map, 0, XHCI_DMA_BYTES);

    u32 *icc = (u32 *)((u8 *)map + 0 * ctx);
    u32 *ep0 = (u32 *)((u8 *)map + 2 * ctx);

    icc[0] = 0;
    icc[1] = (1U << 1);

    bool copied = false;
    if (dev->output_ctx_paddr) {
        void *out_map = arch_phys_map(dev->output_ctx_paddr, XHCI_DMA_BYTES, 0);

        if (out_map) {
            const u32 *out_ep0 = (const u32 *)((const u8 *)out_map + 2 * ctx);
            memcpy(ep0, out_ep0, ctx);
            copied = true;
            arch_phys_unmap(out_map, XHCI_DMA_BYTES);
        }
    }

    if (!copied) {
        _xhci_set_ep_ctx(ep0, XHCI_EP_TYPE_CTRL, dev->max_packet0, dev->ep0_ring.paddr, 8);
    }

    ep0[1] &= ~(0xffffU << 16);
    ep0[1] |= (u32)dev->max_packet0 << 16;

    if (!ep0[2] && !ep0[3]) {
        ep0[2] = (u32)(dev->ep0_ring.paddr & ~0x0fULL) | 1U;
        ep0[3] = (u32)(dev->ep0_ring.paddr >> 32);
    }

    arch_phys_unmap(map, XHCI_DMA_BYTES);
    return true;
}

bool _xhci_prepare_config_ctx(xhci_usb_device_t *dev) {
    if (!dev || !dev->ctrl || !dev->input_ctx_paddr || !dev->ep0_ring.paddr) {
        return false;
    }

    if (!dev->bulk_in_ready && !dev->bulk_out_ready) {
        return false;
    }

    xhci_controller_t *ctrl = dev->ctrl;
    size_t ctx = _ctx_bytes(ctrl);

    void *map = arch_phys_map(dev->input_ctx_paddr, XHCI_DMA_BYTES, 0);
    if (!map) {
        return false;
    }

    memset(map, 0, XHCI_DMA_BYTES);

    u32 *icc = (u32 *)((u8 *)map + 0 * ctx);
    u32 *slot = (u32 *)((u8 *)map + 1 * ctx);
    u32 *ep_out = NULL;
    u32 *ep_in = NULL;

    u8 context_entries = 1;
    u32 add_flags = (1U << 0);

    if (dev->bulk_out_ready && dev->bulk_out_dci) {
        if (dev->bulk_out_dci > context_entries) {
            context_entries = dev->bulk_out_dci;
        }

        add_flags |= (1U << dev->bulk_out_dci);
        ep_out = (u32 *)((u8 *)map + (size_t)(dev->bulk_out_dci + 1) * ctx);
    }

    if (dev->bulk_in_ready && dev->bulk_in_dci) {
        if (dev->bulk_in_dci > context_entries) {
            context_entries = dev->bulk_in_dci;
        }

        add_flags |= (1U << dev->bulk_in_dci);
        ep_in = (u32 *)((u8 *)map + (size_t)(dev->bulk_in_dci + 1) * ctx);
    }

    icc[0] = 0;
    icc[1] = add_flags;

    bool have_output_slot = false;
    void *out_map = arch_phys_map(dev->output_ctx_paddr, XHCI_DMA_BYTES, 0);

    if (out_map) {
        const u32 *out_slot = (const u32 *)((const u8 *)out_map + 1 * ctx);
        memcpy(slot, out_slot, ctx);
        have_output_slot = true;
        arch_phys_unmap(out_map, XHCI_DMA_BYTES);
    }

    if (have_output_slot) {
        slot[0] &= ~(0x1fU << 27);
        slot[0] |= (u32)context_entries << 27;
        slot[1] &= ~(0xffU << 16);
        slot[1] |= (u32)(dev->port & 0xffU) << 16;
    } else {
        _xhci_set_slot_ctx(ctrl, slot, dev->port_speed_id, context_entries, (u8)dev->port);
    }

    if (ep_out) {
        _xhci_set_ep_ctx(
            ep_out,
            XHCI_EP_TYPE_BULK_OUT,
            dev->bulk_out_mps,
            dev->bulk_out_ring.paddr,
            dev->bulk_out_mps
        );
    }

    if (ep_in) {
        _xhci_set_ep_ctx(
            ep_in,
            XHCI_EP_TYPE_BULK_IN,
            dev->bulk_in_mps,
            dev->bulk_in_ring.paddr,
            dev->bulk_in_mps
        );
    }

    arch_phys_unmap(map, XHCI_DMA_BYTES);
    return true;
}

static xhci_trb_t _xhci_make_cmd_trb(u32 type, u8 slot_id, u64 parameter) {
    return (xhci_trb_t){
        .parameter_lo = (u32)(parameter & ~0x0fULL),
        .parameter_hi = (u32)(parameter >> 32),
        .status = 0,
        .control =
            (type << XHCI_TRB_TYPE_SHIFT) |
            ((u32)slot_id << XHCI_TRB_SLOT_ID_SHIFT),
    };
}

static bool _xhci_submit_simple_cmd(
    xhci_controller_t *ctrl,
    u32 type,
    u8 slot_id,
    u64 parameter,
    u8 *out_slot
) {
    xhci_trb_t trb = _xhci_make_cmd_trb(type, slot_id, parameter);
    return _xhci_submit_command(ctrl, &trb, out_slot, XHCI_CMD_TIMEOUT_MS);
}

bool _xhci_cmd_enable_slot(xhci_controller_t *ctrl, u8 *out_slot) {
    return _xhci_submit_simple_cmd(
        ctrl,
        XHCI_TRB_TYPE_ENABLE_SLOT,
        0,
        0,
        out_slot
    );
}

bool _xhci_cmd_disable_slot(xhci_controller_t *ctrl, u8 slot_id) {
    if (!slot_id) {
        return true;
    }

    return _xhci_submit_simple_cmd(
        ctrl,
        XHCI_TRB_TYPE_DISABLE_SLOT,
        slot_id,
        0,
        NULL
    );
}

bool _xhci_cmd_address_device(xhci_controller_t *ctrl, u8 slot_id, u64 input_ctx) {
    return _xhci_submit_simple_cmd(
        ctrl,
        XHCI_TRB_TYPE_ADDRESS_DEVICE,
        slot_id,
        input_ctx,
        NULL
    );
}

bool _xhci_cmd_configure_endpoint(
    xhci_controller_t *ctrl,
    u8 slot_id,
    u64 input_ctx
) {
    return _xhci_submit_simple_cmd(
        ctrl,
        XHCI_TRB_TYPE_CONFIGURE_ENDPOINT,
        slot_id,
        input_ctx,
        NULL
    );
}

bool _xhci_cmd_evaluate_context(
    xhci_controller_t *ctrl,
    u8 slot_id,
    u64 input_ctx
) {
    return _xhci_submit_simple_cmd(
        ctrl,
        XHCI_TRB_TYPE_EVALUATE_CONTEXT,
        slot_id,
        input_ctx,
        NULL
    );
}
