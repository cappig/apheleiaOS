#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

#define FDT_MAGIC 0xd00dfeedU

typedef struct {
    u64 addr;
    u64 size;
} fdt_reg_t;

bool fdt_valid(const void *dtb);
size_t fdt_size(const void *dtb);
bool fdt_boot_cpuid_phys(const void *dtb, u64 *out);

bool fdt_find_memory_reg(const void *dtb, fdt_reg_t *out);

bool fdt_find_compatible_regs(
    const void *dtb,
    const char *compatible,
    fdt_reg_t *out,
    size_t max_regs,
    size_t *out_count
);

bool fdt_find_compatible_irqs(
    const void *dtb,
    const char *compatible,
    u32 *out,
    size_t max_irqs,
    size_t *out_count
);

bool fdt_find_compatible_reg(
    const void *dtb,
    const char *compatible,
    fdt_reg_t *out
);

bool fdt_find_compatible_irq(
    const void *dtb,
    const char *compatible,
    u32 *out
);

bool fdt_find_initrd(const void *dtb, fdt_reg_t *out);
