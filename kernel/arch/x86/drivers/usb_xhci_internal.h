#pragma once

#include <arch/arch.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <sched/scheduler.h>
#include <stdbool.h>
#include <stddef.h>

#include <sys/pci.h>
#include <sys/usb.h>
#include <x86/asm.h>

#define USB_SUBCLASS       0x03
#define USB_PROGIF_XHCI    0x30

#define XHCI_BAR0          0x10
#define XHCI_MMIO_SIZE     0x10000
#define XHCI_MSI_VECTOR    0x41

#define XHCI_MAX_DEVICES       8
#define XHCI_MAX_SCAN_PORTS    32U
#define XHCI_MAX_SCRATCHPADS   512U
#define XHCI_DMA_BYTES         4096U

#define XHCI_CAPLENGTH_OFF     0x00
#define XHCI_HCSPARAMS1_OFF    0x04
#define XHCI_HCSPARAMS2_OFF    0x08
#define XHCI_HCCPARAMS1_OFF    0x10
#define XHCI_DBOFF_OFF         0x14
#define XHCI_RTSOFF_OFF        0x18

#define XHCI_OP_USBCMD_OFF     0x00
#define XHCI_OP_USBSTS_OFF     0x04
#define XHCI_OP_PAGESIZE_OFF   0x08
#define XHCI_OP_CRCR_OFF       0x18
#define XHCI_OP_DCBAAP_OFF     0x30
#define XHCI_OP_CONFIG_OFF     0x38
#define XHCI_OP_PORTSC_BASE    0x400
#define XHCI_OP_PORTSC_STRIDE  0x10

#define XHCI_USBCMD_RUNSTOP    (1U << 0)
#define XHCI_USBCMD_HCRST      (1U << 1)
#define XHCI_USBCMD_INTE       (1U << 2)

#define XHCI_USBSTS_HCH        (1U << 0)
#define XHCI_USBSTS_HSE        (1U << 2)
#define XHCI_USBSTS_EINT       (1U << 3)
#define XHCI_USBSTS_PCD        (1U << 4)
#define XHCI_USBSTS_CNR        (1U << 11)

#define XHCI_CONFIG_MAX_SLOTS_MASK 0xffU

#define XHCI_RT_IR0_OFF        0x20
#define XHCI_IR_IMAN_OFF       0x00
#define XHCI_IR_IMOD_OFF       0x04
#define XHCI_IR_ERSTSZ_OFF     0x08
#define XHCI_IR_ERSTBA_OFF     0x10
#define XHCI_IR_ERDP_OFF       0x18

#define XHCI_IMAN_IP           (1U << 0)
#define XHCI_IMAN_IE           (1U << 1)
#define XHCI_ERDP_EHB          (1ULL << 3)

#define XHCI_PORTSC_CCS        (1U << 0)
#define XHCI_PORTSC_PED        (1U << 1)
#define XHCI_PORTSC_PR         (1U << 4)
#define XHCI_PORTSC_PP         (1U << 9)
#define XHCI_PORTSC_WPR        (1U << 31)
#define XHCI_PORTSC_WCE        (1U << 24)
#define XHCI_PORTSC_WDE        (1U << 25)
#define XHCI_PORTSC_WOE        (1U << 26)
#define XHCI_PORTSC_CHANGE_BITS 0x00fe0000U
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK  0xf
#define XHCI_PORTSC_PRESERVE_BITS (XHCI_PORTSC_PP)

#define XHCI_TRB_TYPE_SHIFT    10
#define XHCI_TRB_TYPE_MASK     0x3fU
#define XHCI_TRB_CYCLE         (1U << 0)
#define XHCI_TRB_ENT           (1U << 1)
#define XHCI_TRB_ISP           (1U << 2)
#define XHCI_TRB_CHAIN         (1U << 4)
#define XHCI_TRB_IOC           (1U << 5)
#define XHCI_TRB_IDT           (1U << 6)

#define XHCI_TRB_TYPE_NORMAL            1U
#define XHCI_TRB_TYPE_SETUP_STAGE       2U
#define XHCI_TRB_TYPE_DATA_STAGE        3U
#define XHCI_TRB_TYPE_STATUS_STAGE      4U
#define XHCI_TRB_TYPE_LINK              6U
#define XHCI_TRB_TYPE_ENABLE_SLOT       9U
#define XHCI_TRB_TYPE_DISABLE_SLOT      10U
#define XHCI_TRB_TYPE_ADDRESS_DEVICE    11U
#define XHCI_TRB_TYPE_CONFIGURE_ENDPOINT 12U
#define XHCI_TRB_TYPE_EVALUATE_CONTEXT  13U
#define XHCI_TRB_TYPE_NOOP_CMD          23U
#define XHCI_TRB_TYPE_TRANSFER_EVENT    32U
#define XHCI_TRB_TYPE_CMD_COMPLETION    33U
#define XHCI_TRB_TYPE_PORT_STATUS_CHANGE 34U

#define XHCI_TRB_SLOT_ID_SHIFT 24
#define XHCI_TRB_EP_ID_SHIFT   16
#define XHCI_LINK_TOGGLE_CYCLE (1U << 1)
#define XHCI_CMD_BSR           (1U << 9)

#define XHCI_CC_SUCCESS        1U
#define XHCI_CC_ENDPOINT_NOT_ENABLED 12U
#define XHCI_CC_SHORT_PACKET   13U

#define XHCI_SETUP_TRT_SHIFT   16
#define XHCI_SETUP_TRT_NONE    0U
#define XHCI_SETUP_TRT_OUT     2U
#define XHCI_SETUP_TRT_IN      3U

#define XHCI_EP_TYPE_ISO_OUT   1U
#define XHCI_EP_TYPE_BULK_OUT  2U
#define XHCI_EP_TYPE_INT_OUT   3U
#define XHCI_EP_TYPE_CTRL      4U
#define XHCI_EP_TYPE_ISO_IN    5U
#define XHCI_EP_TYPE_BULK_IN   6U
#define XHCI_EP_TYPE_INT_IN    7U

#define XHCI_CMD_TIMEOUT_MS   2000
#define XHCI_XFER_TIMEOUT_MS  8000
#define XHCI_HALT_TIMEOUT_MS  500
#define XHCI_CNR_TIMEOUT_MS   8000

typedef struct PACKED {
    u32 parameter_lo;
    u32 parameter_hi;
    u32 status;
    u32 control;
} xhci_trb_t;

typedef struct PACKED {
    u32 segment_base_lo;
    u32 segment_base_hi;
    u32 segment_size;
    u32 reserved;
} xhci_erst_entry_t;

typedef struct {
    u64 paddr;
    u16 trbs;
    u16 enqueue;
    u8 cycle;
} xhci_ring_state_t;

typedef struct xhci_controller xhci_controller_t;

typedef enum {
    XHCI_HEALTH_HEALTHY = 0,
    XHCI_HEALTH_DEGRADED = 1,
    XHCI_HEALTH_OFFLINE = 2,
} xhci_health_t;

typedef struct {
    xhci_controller_t *ctrl;
    size_t hcd_id;
    size_t port;
    u8 slot_id;
    u8 port_speed_id;

    u16 vendor_id;
    u16 product_id;

    u8 device_class;
    u8 device_subclass;
    u8 device_protocol;

    u8 interface_number;
    u8 interface_class;
    u8 interface_subclass;
    u8 interface_protocol;

    u8 config_value;

    u16 max_packet0;

    u8 bulk_in_ep;
    u8 bulk_out_ep;
    u16 bulk_in_mps;
    u16 bulk_out_mps;
    u8 bulk_in_dci;
    u8 bulk_out_dci;

    u64 input_ctx_paddr;
    u64 output_ctx_paddr;

    xhci_ring_state_t ep0_ring;
    xhci_ring_state_t bulk_in_ring;
    xhci_ring_state_t bulk_out_ring;

    bool bulk_in_ready;
    bool bulk_out_ready;
} xhci_usb_device_t;

struct xhci_controller {
    bool used;

    u8 bus;
    u8 slot;
    u8 func;

    u16 vendor_id;
    u16 device_id;

    u8 cap_length;
    u8 max_ports;
    u8 max_slots;
    u8 irq_line;

    u32 db_offset;
    u32 rt_offset;

    bool supports_64bit;
    bool context_64;

    u16 scratchpad_count;
    u16 scratchpad_array_pages;

    u64 mmio_base;

    u64 dcbaa_paddr;
    u64 cmd_ring_paddr;
    u64 event_ring_paddr;
    u64 erst_paddr;
    u64 scratchpad_array_paddr;
    u64 scratchpad_paddrs[XHCI_MAX_SCRATCHPADS];

    xhci_ring_state_t cmd_ring;

    u16 event_ring_trbs;
    u16 event_dequeue;
    u8 event_cycle;

    bool runtime_ready;
    bool commands_healthy;
    xhci_health_t health_state;

    size_t hcd_id;
    volatile u64 irq_seq;

    bool msi_enabled;
    bool irq_enabled;

    volatile int op_lock;
    volatile int event_lock;

    volatile bool cmd_wait_active;
    volatile bool cmd_wait_done;
    u64 cmd_wait_trb;
    u8 cmd_wait_cc;
    u8 cmd_wait_slot;

    volatile bool xfer_wait_active;
    volatile bool xfer_wait_done;
    u64 xfer_wait_trb;
    u8 xfer_wait_slot;
    u8 xfer_wait_ep;
    u8 xfer_wait_cc;
    u32 xfer_wait_residual;

    bool crcr_cycle_one;
    bool crcr_hi_first;
    bool write64_hi_first;
    bool write64_order_tested;
    bool first_fault_logged;

    u64 watchdog_last_irq_seq;
    u32 watchdog_stall_ticks;
    bool event_cycle_sync_pending;

    xhci_usb_device_t *port_devices[XHCI_MAX_SCAN_PORTS + 1];
};

extern xhci_controller_t controllers[XHCI_MAX_DEVICES];
extern size_t controller_count;
extern bool msi_handler_registered;
extern bool legacy_handler_registered;
extern u8 legacy_irq_line;
extern sched_thread_t *xhci_watchdog_thread;
extern volatile bool xhci_watchdog_stop;
extern const usb_hcd_ops_t xhci_hcd_ops;


static inline u32 _read32(const volatile void *base, size_t offset) {
    const volatile u8 *ptr = base;
    return *(const volatile u32 *)(ptr + offset);
}

static inline void _write32(volatile void *base, size_t offset, u32 value) {
    volatile u8 *ptr = base;
    *(volatile u32 *)(ptr + offset) = value;
}

static inline void _write64(volatile void *base, size_t offset, u64 value) {
    _write32(base, offset, (u32)(value & 0xffffffffULL));
    _write32(base, offset + 4, (u32)(value >> 32));
}

static inline u64 _read64(volatile void *base, size_t offset) {
    return (u64)_read32(base, offset) | ((u64)_read32(base, offset + 4) << 32);
}

static inline void _xhci_write64_mmio(
    const xhci_controller_t *ctrl,
    volatile void *base,
    size_t offset,
    u64 value
) {
    if (!base) {
        return;
    }

    if (ctrl && ctrl->write64_hi_first) {
        _write32(base, offset + 4, (u32)(value >> 32));
        _write32(base, offset, (u32)(value & 0xffffffffULL));
        return;
    }

    _write64(base, offset, value);
}

static inline u64 _trb_param(const xhci_trb_t *trb) {
    return (u64)trb->parameter_lo | ((u64)trb->parameter_hi << 32);
}

static inline u32 _trb_type(const xhci_trb_t *trb) {
    return (trb->control >> XHCI_TRB_TYPE_SHIFT) & XHCI_TRB_TYPE_MASK;
}

static inline size_t _ctx_bytes(const xhci_controller_t *ctrl) {
    return ctrl && ctrl->context_64 ? 64U : 32U;
}

static inline u8 _ep_to_dci(u8 endpoint) {
    u8 num = endpoint & USB_ENDPOINT_NUM_MASK;

    if (!num) {
        return 1;
    }

    if (endpoint & USB_ENDPOINT_DIR_IN) {
        return (u8)(num * 2U + 1U);
    }

    return (u8)(num * 2U);
}


bool _xhci_write64_checked(
    xhci_controller_t *ctrl,
    volatile void *base,
    size_t offset,
    u64 value,
    u64 verify_mask
);
bool _wait_bits32(const volatile u32 *reg, u32 mask, bool set, u32 timeout_ms);
void _xhci_op_lock(xhci_controller_t *ctrl);
void _xhci_op_unlock(xhci_controller_t *ctrl);
bool _xhci_alloc_dma_pages(xhci_controller_t *ctrl, size_t pages, u64 *out_paddr);
void _xhci_free_dma_pages(u64 *paddr, size_t pages);
void _xhci_reset_wait_state(xhci_controller_t *ctrl);
bool _xhci_ring_init(xhci_controller_t *ctrl, xhci_ring_state_t *ring, u64 ring_paddr);
bool _xhci_ring_enqueue(
    xhci_controller_t *ctrl,
    xhci_ring_state_t *ring,
    const xhci_trb_t *trb,
    u64 *out_trb_paddr
);
xhci_controller_t *_xhci_find_by_hcd(size_t hcd_id);
bool _xhci_runtime_available(const xhci_controller_t *ctrl);
u32 _xhci_read_usbsts(xhci_controller_t *ctrl);
bool _xhci_ensure_running(xhci_controller_t *ctrl);
void _xhci_set_health_state(
    xhci_controller_t *ctrl,
    xhci_health_t state,
    const char *reason
);
void _xhci_log_fault_snapshot(xhci_controller_t *ctrl, const char *reason);
bool _xhci_update_erdp(xhci_controller_t *ctrl, volatile void *mmio);
bool _xhci_dcbaa_set_entry(xhci_controller_t *ctrl, size_t index, u64 value);
bool _xhci_ring_doorbell(xhci_controller_t *ctrl, u8 db_index, u8 target);
bool _xhci_get_mmio_base(const pci_found_t *node, u64 *out_mmio_base);

bool _xhci_wait_cmd(xhci_controller_t *ctrl, u32 timeout_ms, u8 *out_cc, u8 *out_slot);
bool _xhci_wait_xfer(
    xhci_controller_t *ctrl,
    u32 timeout_ms,
    u8 *out_cc,
    u32 *out_residual
);
bool _xhci_submit_command(
    xhci_controller_t *ctrl,
    const xhci_trb_t *cmd,
    u8 *out_slot,
    u32 timeout_ms
);
bool _xhci_submit_transfer(
    xhci_controller_t *ctrl,
    xhci_usb_device_t *dev,
    xhci_ring_state_t *ring,
    u8 endpoint_id,
    const xhci_trb_t *trbs,
    size_t trb_count,
    size_t event_trb_index,
    size_t expected_data_len,
    bool allow_short,
    u32 timeout_ms,
    size_t *out_actual
);
bool _xhci_cmd_enable_slot(xhci_controller_t *ctrl, u8 *out_slot);
bool _xhci_cmd_disable_slot(xhci_controller_t *ctrl, u8 slot_id);
bool _xhci_cmd_address_device(xhci_controller_t *ctrl, u8 slot_id, u64 input_ctx);
bool _xhci_cmd_configure_endpoint(xhci_controller_t *ctrl, u8 slot_id, u64 input_ctx);
bool _xhci_cmd_evaluate_context(xhci_controller_t *ctrl, u8 slot_id, u64 input_ctx);

void _xhci_ack_port_change_bits(volatile u8 *op, size_t off, u32 portsc);
void _xhci_report_port(
    xhci_controller_t *ctrl,
    volatile u8 *op,
    size_t port,
    bool clear_changes,
    bool log_connected
);
size_t _xhci_scan_ports(
    xhci_controller_t *ctrl,
    volatile u8 *op,
    bool clear_changes,
    bool log_connected
);
bool _xhci_process_events(
    xhci_controller_t *ctrl,
    volatile void *mmio,
    volatile u8 *op,
    bool process_port_events
);
bool _xhci_poll_events(
    xhci_controller_t *ctrl,
    bool process_port_events,
    bool force_event_scan
);
void _xhci_service_interrupts(void);
void _xhci_enable_port_power(xhci_controller_t *ctrl, volatile u8 *op);
void _xhci_log_port_snapshot(xhci_controller_t *ctrl, volatile u8 *op);
size_t _xhci_wait_for_ports(
    xhci_controller_t *ctrl,
    volatile u8 *op,
    u32 timeout_ms
);
bool _xhci_port_reset(xhci_controller_t *ctrl, size_t port, usb_speed_t *out_speed);
u8 _xhci_speed_to_psiv(usb_speed_t speed);

void _xhci_set_slot_ctx(
    xhci_controller_t *ctrl,
    u32 *slot_ctx,
    u8 speed_id,
    u8 context_entries,
    u8 root_port
);
void _xhci_set_ep_ctx(
    u32 *ep_ctx,
    u8 ep_type,
    u16 max_packet,
    u64 dequeue_paddr,
    u16 avg_trb_len
);
bool _xhci_prepare_address_ctx(xhci_usb_device_t *dev);
bool _xhci_prepare_ep0_eval_ctx(xhci_usb_device_t *dev);
bool _xhci_prepare_config_ctx(xhci_usb_device_t *dev);
bool _xhci_control_xfer(
    xhci_usb_device_t *dev,
    const usb_setup_packet_t *setup,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
);
u16 _xhci_default_bulk_mps(const xhci_usb_device_t *dev);
bool _xhci_ensure_bulk_endpoint(xhci_usb_device_t *dev, u8 endpoint);
bool _xhci_bulk_xfer(
    xhci_usb_device_t *dev,
    u8 endpoint,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
);
void _xhci_free_usb_device(xhci_usb_device_t *dev);
void _xhci_release_usb_device(
    xhci_controller_t *ctrl,
    size_t port,
    xhci_usb_device_t *dev
);
void _xhci_release_device(size_t hcd_id, size_t port, void *device_ctx);

bool _xhci_hcd_control_transfer(
    size_t hcd_id,
    size_t port,
    void *device_ctx,
    const usb_transfer_t *transfer,
    size_t *out_actual
);
bool _xhci_hcd_bulk_transfer(
    size_t hcd_id,
    size_t port,
    void *device_ctx,
    const usb_transfer_t *transfer,
    size_t *out_actual
);
bool _xhci_hcd_port_reset(size_t hcd_id, size_t port, usb_speed_t *out_speed);
bool _xhci_hcd_device_open(
    size_t hcd_id,
    size_t port,
    usb_speed_t speed,
    void **out_device_ctx
);
bool _xhci_hcd_set_address(
    size_t hcd_id,
    size_t port,
    void *device_ctx,
    u8 address,
    u16 ep0_mps
);
