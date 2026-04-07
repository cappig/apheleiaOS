#pragma once

#include <base/types.h>

typedef void (*riscv_irq_handler_t)(u32 irq, void *ctx);

const void *riscv_boot_dtb(void);
u64 riscv_boot_hartid(void);
bool riscv_irq_register(u32 irq, riscv_irq_handler_t handler, void *ctx);
void riscv_irq_unregister(u32 irq);
