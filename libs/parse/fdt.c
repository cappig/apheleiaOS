#include "fdt.h"

#include <base/types.h>
#include <stddef.h>
#include <string.h>

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

typedef struct {
    u32 magic;
    u32 totalsize;
    u32 off_dt_struct;
    u32 off_dt_strings;
    u32 off_mem_rsvmap;
    u32 version;
    u32 last_comp_version;
    u32 boot_cpuid_phys;
    u32 size_dt_strings;
    u32 size_dt_struct;
} fdt_header_t;

typedef struct {
    const u8 *dt_struct;
    const u8 *dt_end;
    const char *strings;
    u32 strings_size;
} fdt_view_t;

typedef struct {
    const char *name;
    const u8 *data;
    u32 len;
} fdt_prop_t;

typedef struct {
    u32 addr_cells;
    u32 size_cells;
    u32 child_addr_cells;
    u32 child_size_cells;
    bool compatible;
    bool enabled;
    bool memory;
    bool chosen;
} fdt_node_state_t;

enum {
    FDT_STACK_DEPTH = 16,
    FDT_DEFAULT_ADDR_CELLS = 2,
    FDT_DEFAULT_SIZE_CELLS = 1,
};

static inline u32 fdt_be32(const void *ptr) {
    const u8 *b = ptr;

    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
}

static inline const u8 *fdt_align4(const u8 *ptr) {
    return (const u8 *)(((uintptr_t)ptr + 3U) & ~(uintptr_t)3U);
}

static bool fdt_header(const void *dtb, const fdt_header_t **out_hdr) {
    if (!dtb || !out_hdr) {
        return false;
    }

    const fdt_header_t *hdr = dtb;
    if (fdt_be32(&hdr->magic) != FDT_MAGIC) {
        return false;
    }

    *out_hdr = hdr;
    return true;
}

static bool fdt_lookup_string(const char *strings, u32 size, u32 offset, const char **out) {
    if (!strings || !out || offset >= size) {
        return false;
    }

    const char *name = strings + offset;
    size_t left = size - offset;

    if (!memchr(name, '\0', left)) {
        return false;
    }

    *out = name;
    return true;
}

static size_t fdt_strnlen(const char *str, size_t max_len) {
    size_t len = 0;

    while (len < max_len && str[len]) {
        len++;
    }

    return len;
}

static bool fdt_name_is_memory(const char *name) {
    if (!name || strncmp(name, "memory", 6)) {
        return false;
    }

    return name[6] == '\0' || name[6] == '@';
}

static bool prop_has_string(const void *data, u32 len, const char *needle) {
    if (!data || !len || !needle) {
        return false;
    }

    const char *cur = data;
    const char *end = cur + len;
    size_t needle_len = strlen(needle);

    while (cur < end && *cur) {
        size_t item_len = fdt_strnlen(cur, (size_t)(end - cur));

        if (item_len == needle_len && !memcmp(cur, needle, item_len)) {
            return true;
        }

        cur += item_len + 1;
    }

    return false;
}

static bool prop_status_ok(const fdt_prop_t *prop) {
    return prop_has_string(prop->data, prop->len, "ok") || prop_has_string(prop->data, prop->len, "okay");
}

static bool prop_u32(const fdt_prop_t *prop, u32 *out) {
    if (!prop || prop->len != sizeof(u32) || !out) {
        return false;
    }

    *out = fdt_be32(prop->data);
    return true;
}

static u64 read_cells(const u8 *ptr, u32 cells) {
    u64 value = 0;

    for (u32 i = 0; i < cells; i++) {
        value = (value << 32) | fdt_be32(ptr + i * sizeof(u32));
    }

    return value;
}

static bool fdt_view_init(const void *dtb, fdt_view_t *view) {
    const fdt_header_t *hdr = NULL;

    if (!view || !fdt_header(dtb, &hdr) || !fdt_valid(dtb)) {
        return false;
    }

    u32 off_struct = fdt_be32(&hdr->off_dt_struct);
    u32 off_strings = fdt_be32(&hdr->off_dt_strings);
    u32 size_struct = fdt_be32(&hdr->size_dt_struct);
    u32 size_strings = fdt_be32(&hdr->size_dt_strings);

    view->dt_struct = (const u8 *)dtb + off_struct;
    view->dt_end = view->dt_struct + size_struct;
    view->strings = (const char *)dtb + off_strings;
    view->strings_size = size_strings;

    return true;
}

static bool fdt_read_name(const u8 **cursor, const u8 *end, const char **out_name) {
    const u8 *p = *cursor;
    const char *name = (const char *)p;

    while (p < end && *p) {
        p++;
    }

    if (p >= end) {
        return false;
    }

    p = fdt_align4(p + 1);
    if (p > end) {
        return false;
    }

    *cursor = p;
    *out_name = name;
    return true;
}

static bool fdt_read_prop(const fdt_view_t *view, const u8 **cursor, fdt_prop_t *prop) {
    const u8 *p = *cursor;

    if ((size_t)(view->dt_end - p) < 2 * sizeof(u32)) {
        return false;
    }

    u32 len = fdt_be32(p);
    u32 nameoff = fdt_be32(p + sizeof(u32));
    p += 2 * sizeof(u32);

    if ((size_t)(view->dt_end - p) < len) {
        return false;
    }

    const char *name = NULL;
    if (!fdt_lookup_string(view->strings, view->strings_size, nameoff, &name)) {
        return false;
    }

    prop->name = name;
    prop->data = p;
    prop->len = len;

    p = fdt_align4(p + len);
    if (p > view->dt_end) {
        return false;
    }

    *cursor = p;
    return true;
}

static bool fdt_read_token(const fdt_view_t *view, const u8 **cursor, u32 *token) {
    if (*cursor + sizeof(u32) > view->dt_end) {
        return false;
    }

    *token = fdt_be32(*cursor);
    *cursor += sizeof(u32);
    return true;
}

static void fdt_enter_node(fdt_node_state_t *stack, int depth, const char *name) {
    if (depth == 0) {
        stack[depth].addr_cells = FDT_DEFAULT_ADDR_CELLS;
        stack[depth].size_cells = FDT_DEFAULT_SIZE_CELLS;
        stack[depth].enabled = true;
    } else {
        stack[depth].addr_cells = stack[depth - 1].child_addr_cells;
        stack[depth].size_cells = stack[depth - 1].child_size_cells;
        stack[depth].enabled = stack[depth - 1].enabled;
    }

    stack[depth].child_addr_cells = stack[depth].addr_cells;
    stack[depth].child_size_cells = stack[depth].size_cells;
    stack[depth].compatible = false;
    stack[depth].memory = fdt_name_is_memory(name);
    stack[depth].chosen = depth == 1 && !strcmp(name, "chosen");
}

static bool fdt_push_node(fdt_node_state_t *stack, int *depth, const char *name) {
    (*depth)++;

    if (*depth >= FDT_STACK_DEPTH) {
        return false;
    }

    fdt_enter_node(stack, *depth, name);
    return true;
}

static bool fdt_leave_node(int *depth) {
    if (*depth < 0) {
        return false;
    }

    (*depth)--;
    return true;
}

static bool fdt_note_bus_prop(fdt_node_state_t *node, const fdt_prop_t *prop) {
    if (!strcmp(prop->name, "#address-cells")) {
        return prop_u32(prop, &node->child_addr_cells);
    }

    if (!strcmp(prop->name, "#size-cells")) {
        return prop_u32(prop, &node->child_size_cells);
    }

    if (!strcmp(prop->name, "status")) {
        if (!prop_status_ok(prop)) {
            node->enabled = false;
        }

        return true;
    }

    return true;
}

static bool reg_layout(const fdt_prop_t *prop, u32 addr_cells, u32 size_cells, u32 *entry_size) {
    u32 cells = addr_cells + size_cells;

    if (!cells) {
        return false;
    }

    u32 bytes = cells * sizeof(u32);
    if (!bytes || (prop->len % bytes) != 0) {
        return false;
    }

    *entry_size = bytes;
    return true;
}

static bool initrd_cell_count(u32 len, u32 root_addr_cells, u32 *out_cells) {
    if (len == sizeof(u32)) {
        *out_cells = 1;
        return true;
    }

    if (root_addr_cells <= 2 && len == root_addr_cells * sizeof(u32)) {
        *out_cells = root_addr_cells;
        return true;
    }

    return false;
}

bool fdt_valid(const void *dtb) {
    const fdt_header_t *hdr = NULL;

    if (!fdt_header(dtb, &hdr)) {
        return false;
    }

    u32 totalsize = fdt_be32(&hdr->totalsize);
    u32 off_struct = fdt_be32(&hdr->off_dt_struct);
    u32 off_strings = fdt_be32(&hdr->off_dt_strings);
    u32 size_struct = fdt_be32(&hdr->size_dt_struct);
    u32 size_strings = fdt_be32(&hdr->size_dt_strings);

    if (!totalsize || !size_struct || !size_strings) {
        return false;
    }

    if (off_struct > totalsize || size_struct > totalsize - off_struct) {
        return false;
    }

    if (off_strings > totalsize || size_strings > totalsize - off_strings) {
        return false;
    }

    return true;
}

size_t fdt_size(const void *dtb) {
    const fdt_header_t *hdr = NULL;

    if (!fdt_header(dtb, &hdr) || !fdt_valid(dtb)) {
        return 0;
    }

    return (size_t)fdt_be32(&hdr->totalsize);
}

bool fdt_boot_cpuid_phys(const void *dtb, u64 *out) {
    const fdt_header_t *hdr = NULL;

    if (!out) {
        return false;
    }

    *out = 0;

    if (!fdt_header(dtb, &hdr) || !fdt_valid(dtb)) {
        return false;
    }

    *out = (u64)fdt_be32(&hdr->boot_cpuid_phys);
    return true;
}

bool fdt_has_compatible(const void *dtb, const char *compatible) {
    if (!compatible || !compatible[0]) {
        return false;
    }

    fdt_view_t fdt;
    if (!fdt_view_init(dtb, &fdt)) {
        return false;
    }

    fdt_node_state_t stack[FDT_STACK_DEPTH];
    int depth = -1;

    const u8 *p = fdt.dt_struct;
    u32 token = 0;

    while (fdt_read_token(&fdt, &p, &token)) {

        if (token == FDT_BEGIN_NODE) {
            const char *name = NULL;

            if (!fdt_read_name(&p, fdt.dt_end, &name) || !fdt_push_node(stack, &depth, name)) {
                return false;
            }

            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth < 0) {
                return false;
            }

            if (stack[depth].compatible && stack[depth].enabled) {
                return true;
            }

            (void)fdt_leave_node(&depth);
            continue;
        }

        if (token == FDT_PROP) {
            fdt_prop_t prop;

            if (!fdt_read_prop(&fdt, &p, &prop)) {
                return false;
            }

            if (depth < 0) {
                continue;
            }

            if (!strcmp(prop.name, "compatible")) {
                if (prop_has_string(prop.data, prop.len, compatible)) {
                    stack[depth].compatible = true;
                }
                continue;
            }

            if (!strcmp(prop.name, "status")) {
                if (!prop_status_ok(&prop)) {
                    stack[depth].enabled = false;
                }
                continue;
            }

            continue;
        }

        if (token == FDT_NOP) {
            continue;
        }

        if (token == FDT_END) {
            break;
        }

        return false;
    }

    return false;
}

bool fdt_find_memory_reg(const void *dtb, fdt_reg_t *out) {
    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    fdt_view_t fdt;
    if (!fdt_view_init(dtb, &fdt)) {
        return false;
    }

    fdt_node_state_t stack[FDT_STACK_DEPTH];
    int depth = -1;

    const u8 *p = fdt.dt_struct;
    u32 token = 0;

    while (fdt_read_token(&fdt, &p, &token)) {

        if (token == FDT_BEGIN_NODE) {
            const char *name = NULL;

            if (!fdt_read_name(&p, fdt.dt_end, &name) || !fdt_push_node(stack, &depth, name)) {
                return false;
            }

            continue;
        }

        if (token == FDT_END_NODE) {
            if (!fdt_leave_node(&depth)) {
                return false;
            }

            continue;
        }

        if (token == FDT_PROP) {
            fdt_prop_t prop;

            if (!fdt_read_prop(&fdt, &p, &prop)) {
                return false;
            }

            if (depth < 0) {
                continue;
            }

            fdt_node_state_t *node = &stack[depth];

            if (!fdt_note_bus_prop(node, &prop)) {
                return false;
            }

            if (!strcmp(prop.name, "device_type")) {
                if (prop_has_string(prop.data, prop.len, "memory")) {
                    node->memory = true;
                }
                continue;
            }

            if (strcmp(prop.name, "reg")) {
                continue;
            }

            if (!node->memory || !node->enabled) {
                continue;
            }

            u32 entry_size = 0;
            if (!reg_layout(&prop, node->addr_cells, node->size_cells, &entry_size)) {
                return false;
            }

            const u8 *cur = prop.data;
            for (u32 off = 0; off < prop.len; off += entry_size) {
                fdt_reg_t reg = {
                    .addr = read_cells(cur, node->addr_cells),
                    .size = read_cells(cur + node->addr_cells * sizeof(u32), node->size_cells),
                };

                if (reg.size > out->size) {
                    *out = reg;
                }

                cur += entry_size;
            }

            continue;
        }

        if (token == FDT_NOP) {
            continue;
        }

        if (token == FDT_END) {
            break;
        }

        return false;
    }

    return out->size != 0;
}

bool fdt_find_compatible_regs(
    const void *dtb,
    const char *compatible,
    fdt_reg_t *out,
    size_t max_regs,
    size_t *out_count
) {
    if (out_count) {
        *out_count = 0;
    }

    if (!compatible || (!out && max_regs)) {
        return false;
    }

    fdt_view_t fdt;
    if (!fdt_view_init(dtb, &fdt)) {
        return false;
    }

    fdt_node_state_t stack[FDT_STACK_DEPTH];
    int depth = -1;
    size_t found = 0;

    const u8 *p = fdt.dt_struct;
    u32 token = 0;

    while (fdt_read_token(&fdt, &p, &token)) {

        if (token == FDT_BEGIN_NODE) {
            const char *name = NULL;

            if (!fdt_read_name(&p, fdt.dt_end, &name) || !fdt_push_node(stack, &depth, name)) {
                return false;
            }

            continue;
        }

        if (token == FDT_END_NODE) {
            if (!fdt_leave_node(&depth)) {
                return false;
            }

            continue;
        }

        if (token == FDT_PROP) {
            fdt_prop_t prop;

            if (!fdt_read_prop(&fdt, &p, &prop)) {
                return false;
            }

            if (depth < 0) {
                continue;
            }

            fdt_node_state_t *node = &stack[depth];

            if (!fdt_note_bus_prop(node, &prop)) {
                return false;
            }

            if (!strcmp(prop.name, "compatible")) {
                if (prop_has_string(prop.data, prop.len, compatible)) {
                    node->compatible = true;
                }
                continue;
            }

            if (strcmp(prop.name, "reg")) {
                continue;
            }

            if (!node->compatible || !node->enabled) {
                continue;
            }

            u32 entry_size = 0;
            if (!reg_layout(&prop, node->addr_cells, node->size_cells, &entry_size)) {
                return false;
            }

            const u8 *cur = prop.data;
            u32 entries = prop.len / entry_size;

            for (u32 i = 0; i < entries; i++) {
                if (found < max_regs) {
                    out[found].addr = read_cells(cur, node->addr_cells);
                    out[found].size = read_cells(cur + node->addr_cells * sizeof(u32), node->size_cells);
                }

                found++;
                cur += entry_size;
            }

            continue;
        }

        if (token == FDT_NOP) {
            continue;
        }

        if (token == FDT_END) {
            break;
        }

        return false;
    }

    if (depth != -1) {
        return false;
    }

    if (out_count) {
        *out_count = found > max_regs ? max_regs : found;
    }

    return found > 0;
}

bool fdt_find_compatible_reg(const void *dtb, const char *compatible, fdt_reg_t *out) {
    size_t count = 0;

    if (!out) {
        return false;
    }

    if (!fdt_find_compatible_regs(dtb, compatible, out, 1, &count)) {
        return false;
    }

    return count > 0;
}

bool fdt_find_compatible_irqs(const void *dtb, const char *compatible, u32 *out, size_t max_irqs, size_t *out_count) {
    if (out_count) {
        *out_count = 0;
    }

    if (!compatible || (!out && max_irqs)) {
        return false;
    }

    fdt_view_t fdt;
    if (!fdt_view_init(dtb, &fdt)) {
        return false;
    }

    fdt_node_state_t stack[FDT_STACK_DEPTH];
    int depth = -1;
    size_t found = 0;

    const u8 *p = fdt.dt_struct;
    u32 token = 0;

    while (fdt_read_token(&fdt, &p, &token)) {

        if (token == FDT_BEGIN_NODE) {
            const char *name = NULL;

            if (!fdt_read_name(&p, fdt.dt_end, &name) || !fdt_push_node(stack, &depth, name)) {
                return false;
            }

            continue;
        }

        if (token == FDT_END_NODE) {
            if (!fdt_leave_node(&depth)) {
                return false;
            }

            continue;
        }

        if (token == FDT_PROP) {
            fdt_prop_t prop;

            if (!fdt_read_prop(&fdt, &p, &prop)) {
                return false;
            }

            if (depth < 0) {
                continue;
            }

            fdt_node_state_t *node = &stack[depth];

            if (!fdt_note_bus_prop(node, &prop)) {
                return false;
            }

            if (!strcmp(prop.name, "compatible")) {
                if (prop_has_string(prop.data, prop.len, compatible)) {
                    node->compatible = true;
                }
                continue;
            }

            if (strcmp(prop.name, "interrupts")) {
                continue;
            }

            if (!node->compatible || !node->enabled) {
                continue;
            }

            if ((prop.len % sizeof(u32)) != 0) {
                return false;
            }

            const u8 *cur = prop.data;
            u32 entries = prop.len / sizeof(u32);

            for (u32 i = 0; i < entries; i++) {
                if (found < max_irqs) {
                    out[found] = fdt_be32(cur);
                }

                found++;
                cur += sizeof(u32);
            }

            continue;
        }

        if (token == FDT_NOP) {
            continue;
        }

        if (token == FDT_END) {
            break;
        }

        return false;
    }

    if (depth != -1) {
        return false;
    }

    if (out_count) {
        *out_count = found > max_irqs ? max_irqs : found;
    }

    return found > 0;
}

bool fdt_find_compatible_irq(const void *dtb, const char *compatible, u32 *out) {
    size_t count = 0;

    if (!out) {
        return false;
    }

    if (!fdt_find_compatible_irqs(dtb, compatible, out, 1, &count)) {
        return false;
    }

    return count > 0;
}

bool fdt_find_compatible_u32(const void *dtb, const char *compatible, const char *property, u32 *out) {
    if (!compatible || !property || !out) {
        return false;
    }

    *out = 0;

    fdt_view_t fdt;
    if (!fdt_view_init(dtb, &fdt)) {
        return false;
    }

    fdt_node_state_t stack[FDT_STACK_DEPTH];
    u32 values[FDT_STACK_DEPTH];
    bool have_value[FDT_STACK_DEPTH];
    int depth = -1;

    const u8 *p = fdt.dt_struct;
    u32 token = 0;

    while (fdt_read_token(&fdt, &p, &token)) {

        if (token == FDT_BEGIN_NODE) {
            const char *name = NULL;

            if (!fdt_read_name(&p, fdt.dt_end, &name) || !fdt_push_node(stack, &depth, name)) {
                return false;
            }

            values[depth] = 0;
            have_value[depth] = false;

            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth < 0) {
                return false;
            }

            if (stack[depth].enabled && stack[depth].compatible && have_value[depth]) {
                *out = values[depth];
                return true;
            }

            (void)fdt_leave_node(&depth);
            continue;
        }

        if (token == FDT_PROP) {
            fdt_prop_t prop;

            if (!fdt_read_prop(&fdt, &p, &prop)) {
                return false;
            }

            if (depth < 0) {
                continue;
            }

            fdt_node_state_t *node = &stack[depth];

            if (!fdt_note_bus_prop(node, &prop)) {
                return false;
            }

            if (!strcmp(prop.name, "compatible")) {
                if (prop_has_string(prop.data, prop.len, compatible)) {
                    node->compatible = true;
                }
                continue;
            }

            if (strcmp(prop.name, property)) {
                continue;
            }

            if (!prop_u32(&prop, &values[depth])) {
                return false;
            }

            have_value[depth] = true;
            continue;
        }

        if (token == FDT_NOP) {
            continue;
        }

        if (token == FDT_END) {
            break;
        }

        return false;
    }

    return false;
}

bool fdt_find_initrd(const void *dtb, fdt_reg_t *out) {
    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    fdt_view_t fdt;
    if (!fdt_view_init(dtb, &fdt)) {
        return false;
    }

    fdt_node_state_t stack[FDT_STACK_DEPTH];
    int depth = -1;
    u32 root_addr_cells = FDT_DEFAULT_ADDR_CELLS;
    u64 initrd_start = 0;
    u64 initrd_end = 0;
    bool have_start = false;
    bool have_end = false;

    const u8 *p = fdt.dt_struct;
    u32 token = 0;

    while (fdt_read_token(&fdt, &p, &token)) {

        if (token == FDT_BEGIN_NODE) {
            const char *name = NULL;

            if (!fdt_read_name(&p, fdt.dt_end, &name) || !fdt_push_node(stack, &depth, name)) {
                return false;
            }

            continue;
        }

        if (token == FDT_END_NODE) {
            if (!fdt_leave_node(&depth)) {
                return false;
            }

            continue;
        }

        if (token == FDT_PROP) {
            fdt_prop_t prop;

            if (!fdt_read_prop(&fdt, &p, &prop)) {
                return false;
            }

            if (depth < 0) {
                continue;
            }

            fdt_node_state_t *node = &stack[depth];

            if (!fdt_note_bus_prop(node, &prop)) {
                return false;
            }

            if (!strcmp(prop.name, "#address-cells")) {
                if (depth == 0) {
                    root_addr_cells = node->child_addr_cells;
                }

                continue;
            }

            if (!strcmp(prop.name, "#size-cells")) {
                continue;
            }

            if (!node->chosen) {
                continue;
            }

            if (!strcmp(prop.name, "linux,initrd-start")) {
                u32 cells = 0;

                if (!initrd_cell_count(prop.len, root_addr_cells, &cells)) {
                    return false;
                }

                initrd_start = read_cells(prop.data, cells);
                have_start = true;
                continue;
            }

            if (!strcmp(prop.name, "linux,initrd-end")) {
                u32 cells = 0;

                if (!initrd_cell_count(prop.len, root_addr_cells, &cells)) {
                    return false;
                }

                initrd_end = read_cells(prop.data, cells);
                have_end = true;
                continue;
            }

            continue;
        }

        if (token == FDT_NOP) {
            continue;
        }

        if (token == FDT_END) {
            break;
        }

        return false;
    }

    if (depth != -1 || !have_start || !have_end || initrd_end <= initrd_start) {
        return false;
    }

    out->addr = initrd_start;
    out->size = initrd_end - initrd_start;
    return true;
}

bool fdt_find_timebase_frequency(const void *dtb, u64 *out) {
    if (!out) {
        return false;
    }

    *out = 0;

    fdt_view_t fdt;
    if (!fdt_view_init(dtb, &fdt)) {
        return false;
    }

    int depth = -1;
    bool in_cpus = false;

    const u8 *p = fdt.dt_struct;
    u32 token = 0;

    while (fdt_read_token(&fdt, &p, &token)) {

        if (token == FDT_BEGIN_NODE) {
            const char *name = NULL;

            if (!fdt_read_name(&p, fdt.dt_end, &name)) {
                return false;
            }

            depth++;

            if (depth == 1 && !strcmp(name, "cpus")) {
                in_cpus = true;
            }

            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth < 0) {
                return false;
            }

            if (depth == 1 && in_cpus) {
                in_cpus = false;
            }

            depth--;
            continue;
        }

        if (token == FDT_PROP) {
            fdt_prop_t prop;

            if (!fdt_read_prop(&fdt, &p, &prop)) {
                return false;
            }

            if (!(in_cpus && depth == 1)) {
                continue;
            }

            if (strcmp(prop.name, "timebase-frequency")) {
                continue;
            }

            if (prop.len == sizeof(u32)) {
                *out = (u64)fdt_be32(prop.data);
                return *out != 0;
            }

            if (prop.len == 2U * sizeof(u32)) {
                *out = read_cells(prop.data, 2);
                return *out != 0;
            }

            return false;
        }

        if (token == FDT_NOP) {
            continue;
        }

        if (token == FDT_END) {
            break;
        }

        return false;
    }

    return false;
}

bool fdt_find_model(const void *dtb, char *out, size_t out_len) {
    if (!out || !out_len) {
        return false;
    }

    out[0] = '\0';

    fdt_view_t fdt;
    if (!fdt_view_init(dtb, &fdt)) {
        return false;
    }

    bool have_fallback = false;
    int depth = -1;

    const u8 *p = fdt.dt_struct;
    u32 token = 0;

    while (fdt_read_token(&fdt, &p, &token)) {

        if (token == FDT_BEGIN_NODE) {
            const char *name = NULL;

            if (!fdt_read_name(&p, fdt.dt_end, &name)) {
                return false;
            }

            depth++;
            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth < 0) {
                return false;
            }

            depth--;
            continue;
        }

        if (token == FDT_PROP) {
            fdt_prop_t prop;

            if (!fdt_read_prop(&fdt, &p, &prop)) {
                return false;
            }

            if (depth != 0) {
                continue;
            }

            bool is_model = !strcmp(prop.name, "model");
            bool is_compatible = !strcmp(prop.name, "compatible");

            if (!is_model && !is_compatible) {
                continue;
            }

            size_t len = fdt_strnlen((const char *)prop.data, prop.len);
            if (!len || len >= prop.len) {
                if (is_model) {
                    return false;
                }

                continue;
            }

            size_t copy_len = len;
            if (copy_len >= out_len) {
                copy_len = out_len - 1;
            }

            memcpy(out, prop.data, copy_len);
            out[copy_len] = '\0';

            if (is_model) {
                return true;
            }

            have_fallback = out[0] != '\0';
            continue;
        }

        if (token == FDT_NOP) {
            continue;
        }

        if (token == FDT_END) {
            break;
        }

        return false;
    }

    return have_fallback;
}
