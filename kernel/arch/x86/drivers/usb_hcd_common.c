#include "usb_hcd_common.h"

#include <arch/arch.h>
#include <sched/scheduler.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <x86/asm.h>
#include <x86/irq.h>
#include <x86/mm/physical.h>

#define USB_HCD_PAGE_BYTES 4096U

bool usb_hcd_irq_line_supported(u8 line) {
    return line <= IRQ_SECONDARY_ATA;
}

bool usb_hcd_wait_bits32(const volatile u32 *reg, u32 mask, bool set, u32 timeout_ms) {
    if (!reg) {
        return false;
    }

    u64 start = arch_timer_ticks();
    u64 timeout = ms_to_ticks(timeout_ms);

    for (;;) {
        u32 value = *reg;
        if (((value & mask) != 0) == set) {
            return true;
        }

        if ((arch_timer_ticks() - start) >= timeout) {
            return false;
        }

        if (sched_is_running() && sched_current()) {
            sched_yield();
            continue;
        }

        cpu_pause();
    }
}

bool usb_hcd_alloc_dma_pages(size_t pages, u64 *out_paddr) {
    if (!pages || !out_paddr) {
        return false;
    }

    u64 paddr = (u64)(uintptr_t)alloc_frames(pages);
    if (!paddr) {
        return false;
    }

    if (paddr & (USB_HCD_PAGE_BYTES - 1U)) {
        free_frames((void *)(uintptr_t)paddr, pages);
        return false;
    }

    memset((void *)(uintptr_t)paddr, 0, pages * USB_HCD_PAGE_BYTES);
    *out_paddr = paddr;

    return true;
}

void usb_hcd_free_dma_pages(u64 *paddr, size_t pages) {
    if (!paddr || !*paddr || !pages) {
        return;
    }

    free_frames((void *)(uintptr_t)*paddr, pages);
    *paddr = 0;
}
