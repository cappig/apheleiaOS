#pragma once

#include <base/types.h>
#include <stddef.h>

#include "usb.h"

typedef struct {
    size_t hcd_id;
    size_t port;
    void *device_ctx;
    const usb_hcd_ops_t *ops;
} usb_stdreq_target_t;

bool usb_stdreq_get_descriptor(
    const usb_stdreq_target_t *target,
    u8 dtype,
    u8 dindex,
    u16 langid,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
);

bool usb_stdreq_set_configuration(
    const usb_stdreq_target_t *target,
    u8 config,
    u32 timeout_ms
);
