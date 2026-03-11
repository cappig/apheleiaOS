#include "usb_desc.h"

#include <string.h>

bool usb_desc_parse_identity_from_config(
    const u8 *cfg,
    size_t cfg_len,
    usb_device_identity_t *identity,
    u8 *out_config_value
) {
    if (!cfg || cfg_len < 9 || !identity || !out_config_value) {
        return false;
    }

    *out_config_value = cfg[5];
    if (!*out_config_value) {
        return false;
    }

    usb_device_identity_t parsed = *identity;

    bool in_target_iface = false;
    bool found_iface = false;
    bool found_target_iface = false;

    for (size_t off = 0; off + 2 <= cfg_len;) {
        u8 len = cfg[off + 0];
        u8 type = cfg[off + 1];

        if (!len || off + len > cfg_len) {
            break;
        }

        if (type == USB_DT_INTERFACE && len >= 9) {
            u8 iface_num = cfg[off + 2];
            u8 alt = cfg[off + 3];
            u8 klass = cfg[off + 5];
            u8 sub = cfg[off + 6];
            u8 proto = cfg[off + 7];

            if (!found_iface && alt == 0) {
                found_iface = true;
                parsed.interface_number = iface_num;
                parsed.interface_class = klass;
                parsed.interface_subclass = sub;
                parsed.interface_protocol = proto;
            }

            in_target_iface =
                alt == 0 &&
                klass == USB_CLASS_MASS_STORAGE;

            if (in_target_iface) {
                found_target_iface = true;
                parsed.interface_number = iface_num;
                parsed.interface_class = klass;
                parsed.interface_subclass = sub;
                parsed.interface_protocol = proto;
            }
        } else if (in_target_iface && type == USB_DT_ENDPOINT && len >= 7) {
            u8 ep_addr = cfg[off + 2];
            u8 attrs = cfg[off + 3];
            u16 mps = (u16)cfg[off + 4] | ((u16)cfg[off + 5] << 8);

            if ((attrs & USB_ENDPOINT_XFER_MASK) != USB_ENDPOINT_XFER_BULK) {
                off += len;
                continue;
            }

            if (ep_addr & USB_ENDPOINT_DIR_IN) {
                if (!parsed.bulk_in_ep) {
                    parsed.bulk_in_ep = ep_addr;
                    parsed.bulk_in_max_packet = mps;
                }
            } else {
                if (!parsed.bulk_out_ep) {
                    parsed.bulk_out_ep = ep_addr;
                    parsed.bulk_out_max_packet = mps;
                }
            }
        }

        off += len;
    }

    if (!found_target_iface) {
        parsed.bulk_in_ep = 0;
        parsed.bulk_out_ep = 0;
        parsed.bulk_in_max_packet = 0;
        parsed.bulk_out_max_packet = 0;
    }

    *identity = parsed;
    return true;
}
