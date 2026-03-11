#include "usb_internal.h"

#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "usb_desc.h"
#include "usb_stdreq.h"

#define USB_ENUM_CTRL_TIMEOUT_MS 1000
#define USB_ENUM_CFG_MAX_BYTES   4096

static u16 _usb_enum_default_ep0_mps(usb_speed_t speed) {
    switch (speed) {
    case USB_SPEED_LOW:
        return 8;
    case USB_SPEED_SUPER:
        return 512;
    case USB_SPEED_FULL:
    case USB_SPEED_HIGH:
    default:
        return 64;
    }
}

static bool _usb_enum_attempt(
    const usb_enum_request_t *req,
    usb_enum_result_t *out
) {
    if (
        !req ||
        !out ||
        !req->ops ||
        !req->ops->port_reset ||
        !req->ops->device_open ||
        !req->ops->device_close ||
        !req->ops->set_address ||
        !req->ops->control_transfer
    ) {
        return false;
    }

    usb_speed_t speed = req->speed;
    const char *fail_while = NULL;

    if (!req->ops->port_reset(req->hcd_id, req->port, &speed)) {
        log_debug(
            "USB enum hcd=%zu port=%zu failed while resetting port",
            req->hcd_id,
            req->port
        );
        return false;
    }

    void *device_ctx = NULL;
    if (!req->ops->device_open(req->hcd_id, req->port, speed, &device_ctx) || !device_ctx) {
        log_debug(
            "USB enum hcd=%zu port=%zu failed while opening device context",
            req->hcd_id,
            req->port
        );
        return false;
    }

    usb_stdreq_target_t target = {
        .hcd_id = req->hcd_id,
        .port = req->port,
        .device_ctx = device_ctx,
        .ops = req->ops,
    };

    u8 dev_desc[18] = {0};
    size_t actual = 0;

    bool got_dev_desc8 = usb_stdreq_get_descriptor(
        &target,
        USB_DT_DEVICE,
        0,
        0,
        dev_desc,
        8,
        USB_ENUM_CTRL_TIMEOUT_MS,
        &actual
    );

    if (!got_dev_desc8 || actual < 8 || dev_desc[1] != USB_DT_DEVICE) {
        fail_while = "reading initial device descriptor";
        goto fail;
    }

    u16 ep0_mps = dev_desc[7] ? dev_desc[7] : _usb_enum_default_ep0_mps(speed);

    if (!req->ops->set_address(req->hcd_id, req->port, device_ctx, req->address, ep0_mps)) {
        fail_while = "setting device address";
        goto fail;
    }

    delay_ms(5);

    bool got_dev_desc_full = usb_stdreq_get_descriptor(
        &target,
        USB_DT_DEVICE,
        0,
        0,
        dev_desc,
        sizeof(dev_desc),
        USB_ENUM_CTRL_TIMEOUT_MS,
        &actual
    );

    if (!got_dev_desc_full || actual < sizeof(dev_desc) || dev_desc[1] != USB_DT_DEVICE) {
        fail_while = "reading full device descriptor";
        goto fail;
    }

    usb_device_identity_t identity = {0};
    identity.device_class = dev_desc[4];
    identity.device_subclass = dev_desc[5];
    identity.device_protocol = dev_desc[6];
    identity.vendor_id = (u16)dev_desc[8] | ((u16)dev_desc[9] << 8);
    identity.product_id = (u16)dev_desc[10] | ((u16)dev_desc[11] << 8);

    u8 cfg_hdr[9] = {0};

    bool got_cfg_head = usb_stdreq_get_descriptor(
        &target,
        USB_DT_CONFIG,
        0,
        0,
        cfg_hdr,
        sizeof(cfg_hdr),
        USB_ENUM_CTRL_TIMEOUT_MS,
        &actual
    );

    if (!got_cfg_head || actual < sizeof(cfg_hdr) || cfg_hdr[1] != USB_DT_CONFIG) {
        fail_while = "reading configuration header";
        goto fail;
    }

    u16 cfg_total = (u16)cfg_hdr[2] | ((u16)cfg_hdr[3] << 8);
    if (cfg_total < sizeof(cfg_hdr) || cfg_total > USB_ENUM_CFG_MAX_BYTES) {
        fail_while = "validating configuration length";
        goto fail;
    }

    u8 *cfg = calloc(1, cfg_total);
    if (!cfg) {
        fail_while = "allocating configuration buffer";
        goto fail;
    }

    bool ok = usb_stdreq_get_descriptor(
        &target,
        USB_DT_CONFIG,
        0,
        0,
        cfg,
        cfg_total,
        USB_ENUM_CTRL_TIMEOUT_MS,
        &actual
    );

    if (!ok || actual < cfg_total) {
        fail_while = "reading full configuration descriptor";
        free(cfg);
        goto fail;
    }

    u8 config_value = 0;
    ok = usb_desc_parse_identity_from_config(cfg, cfg_total, &identity, &config_value);
    free(cfg);

    if (!ok || !config_value) {
        fail_while = "parsing configuration descriptors";
        goto fail;
    }

    if (!usb_stdreq_set_configuration(&target, config_value, USB_ENUM_CTRL_TIMEOUT_MS)) {
        fail_while = "setting active configuration";
        goto fail;
    }

    out->identity = identity;
    out->device_ctx = device_ctx;

    return true;

fail:
    if (!fail_while) {
        fail_while = "probing device";
    }

    log_debug(
        "USB enum hcd=%zu port=%zu failed while %s",
        req->hcd_id,
        req->port,
        fail_while
    );

    req->ops->device_close(req->hcd_id, req->port, device_ctx);

    return false;
}

bool usb_enum_run(const usb_enum_request_t *req, usb_enum_result_t *out) {
    if (!req || !out) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    return _usb_enum_attempt(req, out);
}
