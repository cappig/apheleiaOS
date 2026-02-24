#include "usb_stdreq.h"

#include "usb_desc.h"

static bool _usb_stdreq_control(
    const usb_stdreq_target_t *target,
    const usb_setup_packet_t *setup,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
) {
    if (
        !target ||
        !target->ops ||
        !target->ops->control_transfer ||
        !setup
    ) {
        return false;
    }

    usb_transfer_t transfer = {
        .endpoint = {
            .address = 0,
            .transfer_type = USB_XFER_CONTROL,
            .max_packet_size = 0,
            .interval = 0,
        },
        .setup = setup,
        .buffer = buffer,
        .length = length,
        .timeout_ms = timeout_ms,
    };

    return target->ops->control_transfer(
        target->hcd_id,
        target->port,
        target->device_ctx,
        &transfer,
        out_actual
    );
}

bool usb_stdreq_get_descriptor(
    const usb_stdreq_target_t *target,
    u8 dtype,
    u8 dindex,
    u16 langid,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
) {
    if (!buffer || !length) {
        return false;
    }

    usb_setup_packet_t setup = {
        .request_type = USB_REQ_DIR_IN,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (u16)(((u16)dtype << 8) | dindex),
        .index = langid,
        .length = (u16)length,
    };

    return _usb_stdreq_control(target, &setup, buffer, length, timeout_ms, out_actual);
}

bool usb_stdreq_set_configuration(
    const usb_stdreq_target_t *target,
    u8 config,
    u32 timeout_ms
) {
    usb_setup_packet_t setup = {
        .request_type = USB_REQ_DIR_OUT,
        .request = USB_REQ_SET_CONFIGURATION,
        .value = config,
        .index = 0,
        .length = 0,
    };

    size_t actual = 0;
    return _usb_stdreq_control(target, &setup, NULL, 0, timeout_ms, &actual);
}
