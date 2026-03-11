#pragma once

#include <base/types.h>
#include <stddef.h>

bool usb_hcd_irq_line_supported(u8 line);
bool usb_hcd_wait_bits32(const volatile u32 *reg, u32 mask, bool set, u32 timeout_ms);
bool usb_hcd_alloc_dma_pages(size_t pages, u64 *out_paddr);
void usb_hcd_free_dma_pages(u64 *paddr, size_t pages);
