#pragma once

#include <base/types.h>
#include <stddef.h>

typedef enum {
    USB_SPEED_LOW = 1,
    USB_SPEED_FULL = 2,
    USB_SPEED_HIGH = 3,
    USB_SPEED_SUPER = 4,
} usb_speed_t;

typedef enum {
    USB_XFER_CONTROL = 0,
    USB_XFER_ISOCHRONOUS = 1,
    USB_XFER_BULK = 2,
    USB_XFER_INTERRUPT = 3,
} usb_transfer_type_t;

typedef struct PACKED {
    u8 request_type;
    u8 request;
    u16 value;
    u16 index;
    u16 length;
} usb_setup_packet_t;

typedef struct usb_endpoint {
    u8 address;
    usb_transfer_type_t transfer_type;
    u16 max_packet_size;
    u8 interval;
} usb_endpoint_t;

typedef struct usb_transfer {
    usb_endpoint_t endpoint;
    const usb_setup_packet_t *setup;
    void *buffer;
    size_t length;
    u32 timeout_ms;
} usb_transfer_t;

typedef enum {
    USB_HCD_UNKNOWN = 0,
    USB_HCD_XHCI,
} usb_hcd_kind_t;

typedef struct {
    usb_hcd_kind_t kind;
    u8 pci_bus;
    u8 pci_slot;
    u8 pci_func;
    u16 vendor_id;
    u16 device_id;
    size_t max_ports;
    bool irq_driven;
    bool msi_enabled;
} usb_hcd_info_t;

typedef struct {
    u8 device_class;
    u8 device_subclass;
    u8 device_protocol;
    u16 vendor_id;
    u16 product_id;
    u8 interface_number;
    u8 interface_class;
    u8 interface_subclass;
    u8 interface_protocol;
    u8 bulk_in_ep;
    u8 bulk_out_ep;
    u16 bulk_in_max_packet;
    u16 bulk_out_max_packet;
} usb_device_identity_t;

typedef struct usb_device *usb_device_handle_t;

typedef struct usb_hcd_ops {
    bool (*port_reset)(size_t hcd_id, size_t port, usb_speed_t *out_speed);
    bool (*device_open)(
        size_t hcd_id,
        size_t port,
        usb_speed_t speed,
        void **out_device_ctx
    );
    void (*device_close)(size_t hcd_id, size_t port, void *device_ctx);
    bool (*set_address)(
        size_t hcd_id,
        size_t port,
        void *device_ctx,
        u8 address,
        u16 ep0_mps
    );
    bool (*control_transfer)(
        size_t hcd_id,
        size_t port,
        void *device_ctx,
        const usb_transfer_t *transfer,
        size_t *out_actual
    );
    bool (*bulk_transfer)(
        size_t hcd_id,
        size_t port,
        void *device_ctx,
        const usb_transfer_t *transfer,
        size_t *out_actual
    );
} usb_hcd_ops_t;

typedef struct usb_class_driver {
    const char *name;
    bool (*match)(const usb_device_identity_t *identity);
    bool (*attach)(usb_device_handle_t dev);
    void (*detach)(usb_device_handle_t dev);
} usb_class_driver_t;

#define USB_CLASS_PER_INTERFACE 0x00
#define USB_CLASS_MASS_STORAGE  0x08

#define USB_ENDPOINT_DIR_IN   0x80
#define USB_ENDPOINT_DIR_OUT  0x00
#define USB_ENDPOINT_NUM_MASK 0x0f

#define USB_REQ_DIR_IN  0x80
#define USB_REQ_DIR_OUT 0x00

#define USB_MSC_SUBCLASS_SCSI 0x06

#define USB_MSC_SUBCLASS_RBC      0x01
#define USB_MSC_SUBCLASS_ATAPI    0x02
#define USB_MSC_SUBCLASS_QIC_157  0x03
#define USB_MSC_SUBCLASS_UFI      0x04
#define USB_MSC_SUBCLASS_SFF8070I 0x05

#define USB_MSC_PROTO_CBI         0x00
#define USB_MSC_PROTO_CBI_NOINTR  0x01
#define USB_MSC_PROTO_BOT     0x50
#define USB_MSC_PROTO_UAS         0x62


bool usb_register_class_driver(const usb_class_driver_t *driver);
bool usb_register_hcd(
    const usb_hcd_info_t *info,
    const usb_hcd_ops_t *ops,
    size_t *out_hcd_id
);
bool usb_report_port_state(
    size_t hcd_id,
    size_t port,
    bool connected,
    usb_speed_t speed
);
bool usb_schedule_port_enumeration(size_t hcd_id, size_t port);
bool usb_wait_for_boot_enumeration(u32 timeout_ms);
size_t usb_device_hcd_id(usb_device_handle_t dev);
size_t usb_device_port(usb_device_handle_t dev);
const usb_device_identity_t *usb_device_identity(usb_device_handle_t dev);
bool usb_device_control_transfer(
    usb_device_handle_t dev,
    const usb_setup_packet_t *setup,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
);
bool usb_device_bulk_transfer(
    usb_device_handle_t dev,
    u8 endpoint,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
);
bool usb_identify_device(
    size_t hcd_id,
    size_t port,
    const usb_device_identity_t *identity
);
bool usb_control_transfer(
    size_t hcd_id,
    size_t port,
    const usb_setup_packet_t *setup,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
);
bool usb_bulk_transfer(
    size_t hcd_id,
    size_t port,
    u8 endpoint,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
);
size_t usb_connected_device_count(void);

bool usb_init(void);
