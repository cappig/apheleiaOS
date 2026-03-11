#pragma once

#include <base/types.h>
#include <stddef.h>

#include "usb.h"

#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_SET_CONFIGURATION 0x09

#define USB_DT_DEVICE    1
#define USB_DT_CONFIG    2
#define USB_DT_INTERFACE 4
#define USB_DT_ENDPOINT  5

#define USB_ENDPOINT_XFER_MASK 0x03
#define USB_ENDPOINT_XFER_BULK 0x02


bool usb_desc_parse_identity_from_config(
    const u8 *cfg,
    size_t cfg_len,
    usb_device_identity_t *identity,
    u8 *out_config_value
);
