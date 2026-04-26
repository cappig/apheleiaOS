#include "fdt.h"

#include <base/types.h>
#include <stddef.h>
#include <string.h>

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

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
    const u8 *b = (const u8 *)ptr;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) |
           ((u32)b[3]);
}

static inline const u8 *fdt_align4(const u8 *ptr) {
    return (const u8 *)(((uintptr_t)ptr + 3u) & ~(uintptr_t)3u);
}

static bool fdt_header(const void *dtb, const fdt_header_t **out_hdr) {
    if (!dtb || !out_hdr) {
        return false;
    }

    const fdt_header_t *hdr = (const fdt_header_t *)dtb;
    if (fdt_be32(&hdr->magic) != FDT_MAGIC) {
        return false;
    }

    *out_hdr = hdr;
    return true;
}

static bool
fdt_lookup_string(const char *strings, u32 size, u32 offset, const char **out) {
    if (!strings || !out || offset >= size) {
        return false;
    }

    const char *name = strings + offset;
    size_t remaining = size - offset;

    if (!memchr(name, '\0', remaining)) {
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
    if (!name) {
        return false;
    }

    if (strncmp(name, "memory", 6)) {
        return false;
    }

    return name[6] == '\0' || name[6] == '@';
}

static bool prop_has_string(
    const void *data,
    u32 len,
    const char *needle
) {
    if (!data || !len || !needle) {
        return false;
    }

    const char *ptr = (const char *)data;
    const char *end = ptr + len;
    size_t needle_len = strlen(needle);

    while (ptr < end && *ptr) {
        size_t item_len = fdt_strnlen(ptr, (size_t)(end - ptr));

        if (item_len == needle_len && !memcmp(ptr, needle, item_len)) {
            return true;
        }

        ptr += item_len + 1;
    }

    return false;
}

static u64 read_cells(const u8 *ptr, u32 cells) {
    u64 value = 0;
    for (u32 i = 0; i < cells; i++) {
        value = (value << 32) | fdt_be32(ptr + i * 4);
    }
    return value;
}

static bool prop_status_ok(const char *data, u32 len) {
    return prop_has_string(data, len, "ok") || prop_has_string(data, len, "okay");
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
    if (off_struct + size_struct > totalsize) {
        return false;
    }
    if (off_strings + size_strings > totalsize) {
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

    const fdt_header_t *hdr = NULL;
    if (!fdt_header(dtb, &hdr) || !fdt_valid(dtb)) {
        return false;
    }

    u32 off_struct = fdt_be32(&hdr->off_dt_struct);
    u32 off_strings = fdt_be32(&hdr->off_dt_strings);
    u32 size_struct = fdt_be32(&hdr->size_dt_struct);
    u32 size_strings = fdt_be32(&hdr->size_dt_strings);

    const u8 *struct_ptr = (const u8 *)dtb + off_struct;
    const u8 *struct_end = struct_ptr + size_struct;
    const char *strings = (const char *)dtb + off_strings;

    fdt_node_state_t stack[FDT_STACK_DEPTH];
    int depth = -1;

    const u8 *p = struct_ptr;
    while (p + 4 <= struct_end) {
        u32 token = fdt_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t len = 0;
            while (p + len < struct_end && name[len]) {
                len++;
            }
            if (p + len >= struct_end) {
                return false;
            }
            p += len + 1;
            p = fdt_align4(p);

            depth++;
            if (depth >= FDT_STACK_DEPTH) {
                return false;
            }

            if (depth == 0) {
                stack[depth].enabled = true;
            } else {
                stack[depth].enabled = stack[depth - 1].enabled;
            }
            stack[depth].compatible = false;
            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth < 0) {
                return false;
            }

            if (stack[depth].compatible && stack[depth].enabled) {
                return true;
            }

            depth--;
            continue;
        }

        if (token == FDT_PROP) {
            if (p + 8 > struct_end) {
                return false;
            }

            u32 len = fdt_be32(p);
            u32 nameoff = fdt_be32(p + 4);
            p += 8;

            const u8 *data = p;
            if (p + len > struct_end) {
                return false;
            }
            p += len;
            p = fdt_align4(p);

            if (depth < 0) {
                continue;
            }

            const char *pname = NULL;
            if (!fdt_lookup_string(strings, size_strings, nameoff, &pname)) {
                return false;
            }

            if (!strcmp(pname, "compatible")) {
                if (prop_has_string(data, len, compatible)) {
                    stack[depth].compatible = true;
                }
                continue;
            }

            if (!strcmp(pname, "status")) {
                if (!prop_status_ok((const char *)data, len)) {
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

    const fdt_header_t *hdr = NULL;
    if (!fdt_header(dtb, &hdr) || !fdt_valid(dtb)) {
        return false;
    }

    u32 off_struct = fdt_be32(&hdr->off_dt_struct);
    u32 off_strings = fdt_be32(&hdr->off_dt_strings);
    u32 size_struct = fdt_be32(&hdr->size_dt_struct);
    u32 size_strings = fdt_be32(&hdr->size_dt_strings);

    const u8 *struct_ptr = (const u8 *)dtb + off_struct;
    const u8 *struct_end = struct_ptr + size_struct;
    const char *strings = (const char *)dtb + off_strings;

    fdt_node_state_t stack[FDT_STACK_DEPTH];
    int depth = -1;

    const u8 *p = struct_ptr;
    while (p + 4 <= struct_end) {
        u32 token = fdt_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t len = 0;
            while (p + len < struct_end && name[len]) {
                len++;
            }
            if (p + len >= struct_end) {
                return false;
            }
            p += len + 1;
            p = fdt_align4(p);

            depth++;
            if (depth >= FDT_STACK_DEPTH) {
                return false;
            }

            if (depth == 0) {
                stack[depth].addr_cells = FDT_DEFAULT_ADDR_CELLS;
                stack[depth].size_cells = FDT_DEFAULT_SIZE_CELLS;
                stack[depth].enabled = true;
            } else {
                stack[depth] = stack[depth - 1];
                stack[depth].addr_cells = stack[depth - 1].child_addr_cells;
                stack[depth].size_cells = stack[depth - 1].child_size_cells;
                stack[depth].enabled = stack[depth - 1].enabled;
            }

            stack[depth].child_addr_cells = stack[depth].addr_cells;
            stack[depth].child_size_cells = stack[depth].size_cells;
            stack[depth].compatible = false;
            stack[depth].memory = fdt_name_is_memory(name);
            stack[depth].chosen = depth == 1 && !strcmp(name, "chosen");
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
            if (p + 8 > struct_end) {
                return false;
            }

            u32 len = fdt_be32(p);
            u32 nameoff = fdt_be32(p + 4);
            p += 8;

            const u8 *data = p;
            if (p + len > struct_end) {
                return false;
            }
            p += len;
            p = fdt_align4(p);

            if (depth < 0) {
                continue;
            }

            const char *pname = NULL;
            if (!fdt_lookup_string(strings, size_strings, nameoff, &pname)) {
                return false;
            }

            if (!strcmp(pname, "#address-cells")) {
                if (len != sizeof(u32)) {
                    return false;
                }
                stack[depth].child_addr_cells = fdt_be32(data);
                continue;
            }

            if (!strcmp(pname, "#size-cells")) {
                if (len != sizeof(u32)) {
                    return false;
                }
                stack[depth].child_size_cells = fdt_be32(data);
                continue;
            }

            if (!strcmp(pname, "device_type")) {
                if (prop_has_string(data, len, "memory")) {
                    stack[depth].memory = true;
                }
                continue;
            }

            if (!strcmp(pname, "status")) {
                if (!prop_status_ok((const char *)data, len)) {
                    stack[depth].enabled = false;
                }
                continue;
            }

            if (!strcmp(pname, "reg")) {
                if (!stack[depth].memory || !stack[depth].enabled) {
                    continue;
                }

                u32 addr_cells = stack[depth].addr_cells;
                u32 size_cells = stack[depth].size_cells;
                u32 cells = addr_cells + size_cells;
                if (!cells) {
                    continue;
                }

                u32 entry_size = cells * sizeof(u32);
                if (!entry_size || (len % entry_size) != 0) {
                    return false;
                }

                const u8 *cur = data;
                for (u32 off = 0; off < len; off += entry_size) {
                    fdt_reg_t reg = {
                        .addr = read_cells(cur, addr_cells),
                        .size = read_cells(cur + addr_cells * sizeof(u32), size_cells),
                    };
                    if (reg.size > out->size) {
                        *out = reg;
                    }
                    cur += entry_size;
                }
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

    const fdt_header_t *hdr = NULL;
    if (!fdt_header(dtb, &hdr) || !fdt_valid(dtb)) {
        return false;
    }

    u32 off_struct = fdt_be32(&hdr->off_dt_struct);
    u32 off_strings = fdt_be32(&hdr->off_dt_strings);
    u32 size_struct = fdt_be32(&hdr->size_dt_struct);
    u32 size_strings = fdt_be32(&hdr->size_dt_strings);

    const u8 *struct_ptr = (const u8 *)dtb + off_struct;
    const u8 *struct_end = struct_ptr + size_struct;
    const char *strings = (const char *)dtb + off_strings;

    fdt_node_state_t stack[FDT_STACK_DEPTH];

    int depth = -1;
    size_t found = 0;

    const u8 *p = struct_ptr;
    while (p + 4 <= struct_end) {
        u32 token = fdt_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t len = 0;
            while (p + len < struct_end && name[len]) {
                len++;
            }
            if (p + len >= struct_end) {
                return false;
            }
            p += len + 1;
            p = fdt_align4(p);

            depth++;
            if (depth >= FDT_STACK_DEPTH) {
                return false;
            }

            if (depth == 0) {
                stack[depth].addr_cells = FDT_DEFAULT_ADDR_CELLS;
                stack[depth].size_cells = FDT_DEFAULT_SIZE_CELLS;
            } else {
                stack[depth].addr_cells = stack[depth - 1].child_addr_cells;
                stack[depth].size_cells = stack[depth - 1].child_size_cells;
            }

            stack[depth].child_addr_cells = stack[depth].addr_cells;
            stack[depth].child_size_cells = stack[depth].size_cells;
            stack[depth].compatible = false;
            stack[depth].enabled = true;
            stack[depth].chosen = depth == 1 && !strcmp(name, "chosen");

            (void)name;
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
            if (p + 8 > struct_end) {
                return false;
            }
            u32 len = fdt_be32(p);
            u32 nameoff = fdt_be32(p + 4);
            p += 8;
            const u8 *data = p;
            if (p + len > struct_end) {
                return false;
            }
            p += len;
            p = fdt_align4(p);

            if (depth < 0) {
                continue;
            }

            const char *pname = NULL;
            if (!fdt_lookup_string(strings, size_strings, nameoff, &pname)) {
                return false;
            }

            if (!strcmp(pname, "#address-cells")) {
                if (len != sizeof(u32)) {
                    return false;
                }
                stack[depth].child_addr_cells = fdt_be32(data);
                continue;
            }

            if (!strcmp(pname, "#size-cells")) {
                if (len != sizeof(u32)) {
                    return false;
                }
                stack[depth].child_size_cells = fdt_be32(data);
                continue;
            }

            if (!strcmp(pname, "compatible")) {
                if (prop_has_string(data, len, compatible)) {
                    stack[depth].compatible = true;
                }
                continue;
            }

            if (!strcmp(pname, "status")) {
                if (!prop_status_ok((const char *)data, len)) {
                    stack[depth].enabled = false;
                }
                continue;
            }

            if (!strcmp(pname, "reg")) {
                if (!stack[depth].compatible || !stack[depth].enabled) {
                    continue;
                }

                u32 addr_cells = stack[depth].addr_cells;
                u32 size_cells = stack[depth].size_cells;
                u32 cells = addr_cells + size_cells;
                if (!cells) {
                    continue;
                }

                u32 entry_size = cells * sizeof(u32);
                if (!entry_size || (len % entry_size) != 0) {
                    return false;
                }

                u32 entries = len / entry_size;
                const u8 *cur = data;
                for (u32 i = 0; i < entries; i++) {
                    if (found < max_regs) {
                        out[found].addr = read_cells(cur, addr_cells);
                        out[found].size = read_cells(
                            cur + addr_cells * sizeof(u32),
                            size_cells
                        );
                    }
                    found++;
                    cur += entry_size;
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

    if (depth != -1) {
        return false;
    }

    if (out_count) {
        *out_count = (found > max_regs) ? max_regs : found;
    }

    return found > 0;
}

bool fdt_find_compatible_reg(
    const void *dtb,
    const char *compatible,
    fdt_reg_t *out
) {
    size_t count = 0;
    if (!out) {
        return false;
    }

    if (!fdt_find_compatible_regs(dtb, compatible, out, 1, &count)) {
        return false;
    }

    return count > 0;
}

bool fdt_find_compatible_irqs(
    const void *dtb,
    const char *compatible,
    u32 *out,
    size_t max_irqs,
    size_t *out_count
) {
    if (out_count) {
        *out_count = 0;
    }
    if (!compatible || (!out && max_irqs)) {
        return false;
    }

    const fdt_header_t *hdr = NULL;
    if (!fdt_header(dtb, &hdr) || !fdt_valid(dtb)) {
        return false;
    }

    u32 off_struct = fdt_be32(&hdr->off_dt_struct);
    u32 off_strings = fdt_be32(&hdr->off_dt_strings);
    u32 size_struct = fdt_be32(&hdr->size_dt_struct);
    u32 size_strings = fdt_be32(&hdr->size_dt_strings);

    const u8 *struct_ptr = (const u8 *)dtb + off_struct;
    const u8 *struct_end = struct_ptr + size_struct;
    const char *strings = (const char *)dtb + off_strings;

    fdt_node_state_t stack[FDT_STACK_DEPTH];

    int depth = -1;
    size_t found = 0;

    const u8 *p = struct_ptr;
    while (p + 4 <= struct_end) {
        u32 token = fdt_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t len = 0;
            while (p + len < struct_end && name[len]) {
                len++;
            }
            if (p + len >= struct_end) {
                return false;
            }
            p += len + 1;
            p = fdt_align4(p);

            depth++;
            if (depth >= FDT_STACK_DEPTH) {
                return false;
            }

            if (depth == 0) {
                stack[depth].addr_cells = FDT_DEFAULT_ADDR_CELLS;
                stack[depth].size_cells = FDT_DEFAULT_SIZE_CELLS;
            } else {
                stack[depth].addr_cells = stack[depth - 1].child_addr_cells;
                stack[depth].size_cells = stack[depth - 1].child_size_cells;
            }

            stack[depth].child_addr_cells = stack[depth].addr_cells;
            stack[depth].child_size_cells = stack[depth].size_cells;
            stack[depth].compatible = false;
            stack[depth].enabled = true;
            stack[depth].chosen = depth == 1 && !strcmp(name, "chosen");

            (void)name;
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
            if (p + 8 > struct_end) {
                return false;
            }
            u32 len = fdt_be32(p);
            u32 nameoff = fdt_be32(p + 4);
            p += 8;
            const u8 *data = p;
            if (p + len > struct_end) {
                return false;
            }
            p += len;
            p = fdt_align4(p);

            if (depth < 0) {
                continue;
            }

            const char *pname = NULL;
            if (!fdt_lookup_string(strings, size_strings, nameoff, &pname)) {
                return false;
            }

            if (!strcmp(pname, "#address-cells")) {
                if (len != sizeof(u32)) {
                    return false;
                }
                stack[depth].child_addr_cells = fdt_be32(data);
                continue;
            }

            if (!strcmp(pname, "#size-cells")) {
                if (len != sizeof(u32)) {
                    return false;
                }
                stack[depth].child_size_cells = fdt_be32(data);
                continue;
            }

            if (!strcmp(pname, "compatible")) {
                if (prop_has_string(data, len, compatible)) {
                    stack[depth].compatible = true;
                }
                continue;
            }

            if (!strcmp(pname, "status")) {
                if (!prop_status_ok((const char *)data, len)) {
                    stack[depth].enabled = false;
                }
                continue;
            }

            if (!strcmp(pname, "interrupts")) {
                if (!stack[depth].compatible || !stack[depth].enabled) {
                    continue;
                }

                if ((len % sizeof(u32)) != 0) {
                    return false;
                }

                const u8 *cur = data;
                for (u32 i = 0; i < len / sizeof(u32); i++) {
                    if (found < max_irqs) {
                        out[found] = fdt_be32(cur);
                    }
                    found++;
                    cur += sizeof(u32);
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

    if (depth != -1) {
        return false;
    }

    if (out_count) {
        *out_count = (found > max_irqs) ? max_irqs : found;
    }

    return found > 0;
}

bool fdt_find_compatible_irq(
    const void *dtb,
    const char *compatible,
    u32 *out
) {
    size_t count = 0;
    if (!out) {
        return false;
    }

    if (!fdt_find_compatible_irqs(dtb, compatible, out, 1, &count)) {
        return false;
    }

    return count > 0;
}

bool fdt_find_initrd(const void *dtb, fdt_reg_t *out) {
    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    const fdt_header_t *hdr = NULL;
    if (!fdt_header(dtb, &hdr) || !fdt_valid(dtb)) {
        return false;
    }

    u32 off_struct = fdt_be32(&hdr->off_dt_struct);
    u32 off_strings = fdt_be32(&hdr->off_dt_strings);
    u32 size_struct = fdt_be32(&hdr->size_dt_struct);
    u32 size_strings = fdt_be32(&hdr->size_dt_strings);

    const u8 *struct_ptr = (const u8 *)dtb + off_struct;
    const u8 *struct_end = struct_ptr + size_struct;
    const char *strings = (const char *)dtb + off_strings;

    fdt_node_state_t stack[FDT_STACK_DEPTH];
    int depth = -1;
    u32 root_addr_cells = FDT_DEFAULT_ADDR_CELLS;
    u64 initrd_start = 0;
    u64 initrd_end = 0;
    bool have_start = false;
    bool have_end = false;

    const u8 *p = struct_ptr;
    while (p + 4 <= struct_end) {
        u32 token = fdt_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t len = 0;
            while (p + len < struct_end && name[len]) {
                len++;
            }
            if (p + len >= struct_end) {
                return false;
            }
            p += len + 1;
            p = fdt_align4(p);

            depth++;
            if (depth >= FDT_STACK_DEPTH) {
                return false;
            }

            if (depth == 0) {
                stack[depth].addr_cells = FDT_DEFAULT_ADDR_CELLS;
                stack[depth].size_cells = FDT_DEFAULT_SIZE_CELLS;
            } else {
                stack[depth].addr_cells = stack[depth - 1].child_addr_cells;
                stack[depth].size_cells = stack[depth - 1].child_size_cells;
            }

            stack[depth].child_addr_cells = stack[depth].addr_cells;
            stack[depth].child_size_cells = stack[depth].size_cells;
            stack[depth].compatible = false;
            stack[depth].enabled = true;
            stack[depth].memory = false;
            stack[depth].chosen = depth == 1 && !strcmp(name, "chosen");
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
            if (p + 8 > struct_end) {
                return false;
            }
            u32 len = fdt_be32(p);
            u32 nameoff = fdt_be32(p + 4);
            p += 8;
            const u8 *data = p;
            if (p + len > struct_end) {
                return false;
            }
            p += len;
            p = fdt_align4(p);

            if (depth < 0) {
                continue;
            }

            const char *pname = NULL;
            if (!fdt_lookup_string(strings, size_strings, nameoff, &pname)) {
                return false;
            }

            if (!strcmp(pname, "#address-cells")) {
                if (len != sizeof(u32)) {
                    return false;
                }
                stack[depth].child_addr_cells = fdt_be32(data);
                if (depth == 0) {
                    root_addr_cells = stack[depth].child_addr_cells;
                }
                continue;
            }

            if (!strcmp(pname, "#size-cells")) {
                if (len != sizeof(u32)) {
                    return false;
                }
                stack[depth].child_size_cells = fdt_be32(data);
                continue;
            }

            if (!stack[depth].chosen) {
                continue;
            }

            if (!strcmp(pname, "linux,initrd-start")) {
                u32 initrd_cells = 0;

                if (len == sizeof(u32)) {
                    initrd_cells = 1;
                } else if (root_addr_cells <= 2 &&
                           len == root_addr_cells * sizeof(u32)) {
                    initrd_cells = root_addr_cells;
                } else {
                    return false;
                }

                initrd_start = read_cells(data, initrd_cells);
                have_start = true;
                continue;
            }

            if (!strcmp(pname, "linux,initrd-end")) {
                u32 initrd_cells = 0;

                if (len == sizeof(u32)) {
                    initrd_cells = 1;
                } else if (root_addr_cells <= 2 &&
                           len == root_addr_cells * sizeof(u32)) {
                    initrd_cells = root_addr_cells;
                } else {
                    return false;
                }

                initrd_end = read_cells(data, initrd_cells);
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

    const fdt_header_t *hdr = NULL;
    if (!fdt_header(dtb, &hdr) || !fdt_valid(dtb)) {
        return false;
    }

    u32 off_struct = fdt_be32(&hdr->off_dt_struct);
    u32 off_strings = fdt_be32(&hdr->off_dt_strings);
    u32 size_struct = fdt_be32(&hdr->size_dt_struct);
    u32 size_strings = fdt_be32(&hdr->size_dt_strings);

    const u8 *struct_ptr = (const u8 *)dtb + off_struct;
    const u8 *struct_end = struct_ptr + size_struct;
    const char *strings = (const char *)dtb + off_strings;

    int depth = -1;
    bool in_cpus = false;

    const u8 *p = struct_ptr;
    while (p + 4 <= struct_end) {
        u32 token = fdt_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t len = 0;
            while (p + len < struct_end && name[len]) {
                len++;
            }
            if (p + len >= struct_end) {
                return false;
            }

            p += len + 1;
            p = fdt_align4(p);

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
            if (p + 8 > struct_end) {
                return false;
            }

            u32 len = fdt_be32(p);
            u32 nameoff = fdt_be32(p + 4);
            p += 8;

            const u8 *data = p;
            if (p + len > struct_end) {
                return false;
            }
            p += len;
            p = fdt_align4(p);

            if (!(in_cpus && depth == 1)) {
                continue;
            }

            const char *pname = NULL;
            if (!fdt_lookup_string(strings, size_strings, nameoff, &pname)) {
                return false;
            }

            if (strcmp(pname, "timebase-frequency")) {
                continue;
            }

            if (len == sizeof(u32)) {
                *out = (u64)fdt_be32(data);
                return *out != 0;
            }

            if (len == (2U * sizeof(u32))) {
                *out = read_cells(data, 2);
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

    const fdt_header_t *hdr = NULL;
    if (!fdt_header(dtb, &hdr) || !fdt_valid(dtb)) {
        return false;
    }

    u32 off_struct = fdt_be32(&hdr->off_dt_struct);
    u32 off_strings = fdt_be32(&hdr->off_dt_strings);
    u32 size_struct = fdt_be32(&hdr->size_dt_struct);
    u32 size_strings = fdt_be32(&hdr->size_dt_strings);

    const u8 *struct_ptr = (const u8 *)dtb + off_struct;
    const u8 *struct_end = struct_ptr + size_struct;
    const char *strings = (const char *)dtb + off_strings;

    bool have_fallback = false;
    int depth = -1;

    const u8 *p = struct_ptr;
    while (p + 4 <= struct_end) {
        u32 token = fdt_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t len = 0;
            while (p + len < struct_end && name[len]) {
                len++;
            }
            if (p + len >= struct_end) {
                return false;
            }

            p += len + 1;
            p = fdt_align4(p);
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
            if (p + 8 > struct_end) {
                return false;
            }

            u32 len = fdt_be32(p);
            u32 nameoff = fdt_be32(p + 4);
            p += 8;

            const u8 *data = p;
            if (p + len > struct_end) {
                return false;
            }
            p += len;
            p = fdt_align4(p);

            if (depth != 0) {
                continue;
            }

            const char *pname = NULL;
            if (!fdt_lookup_string(strings, size_strings, nameoff, &pname)) {
                return false;
            }

            if (strcmp(pname, "model") && strcmp(pname, "compatible")) {
                continue;
            }

            size_t slen = fdt_strnlen((const char *)data, len);
            if (!slen || slen >= len) {
                if (!strcmp(pname, "model")) {
                    return false;
                }
                continue;
            }

            size_t copy_len = slen;
            if (copy_len >= out_len) {
                copy_len = out_len - 1;
            }

            memcpy(out, data, copy_len);
            out[copy_len] = '\0';

            if (!strcmp(pname, "model")) {
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
