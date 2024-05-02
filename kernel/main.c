#include <alloc/bitmap.h>
#include <alloc/global.h>
#include <base/addr.h>
#include <base/attributes.h>
#include <base/types.h>
#include <boot/proto.h>
#include <gfx/state.h>
#include <log/log.h>
#include <x86/asm.h>
#include <x86/paging.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/pic.h"
#include "arch/ps2.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "mem/virtual.h"
#include "video/tty.h"


NORETURN void _kern_entry(boot_handoff* handoff) {
    log_init(&puts);

    if (handoff->magic != BOOT_MAGIC)
        panic("Kernel booted with invalid args!");

    gdt_init();

    vmm_init();

    reclaim_boot_map(&handoff->mmap);
    pmm_init(&handoff->mmap);

    heap_init();
    galloc_init();

    tty_init(&handoff->graphics);

    idt_init();
    pic_init();

    init_ps2_kbd();

    enable_interrupts();

    log_info(ALPHA_ASCII);

    halt();
    __builtin_unreachable();
}
