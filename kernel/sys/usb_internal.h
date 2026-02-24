#pragma once

#include <base/types.h>

#include "usb.h"

typedef struct {
    size_t hcd_id;
    size_t port;
    usb_speed_t speed;
    u8 address;
    const usb_hcd_ops_t *ops;
} usb_enum_request_t;

typedef struct {
    void *device_ctx;
    usb_device_identity_t identity;
} usb_enum_result_t;

bool usb_enum_run(const usb_enum_request_t *req, usb_enum_result_t *out);
