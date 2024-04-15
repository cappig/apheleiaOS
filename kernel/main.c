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

#include "mem/heap.h"
#include "mem/physical.h"
#include "mem/virtual.h"
#include "video/tty.h"


NORETURN void _kern_entry(boot_handoff* handoff) {
    log_init(&puts);

    if (handoff->magic != BOOT_MAGIC)
        panic("Kernel booted with invalid args!");

    vmm_init();

    reclaim_boot_map(&handoff->mmap);
    pmm_init(&handoff->mmap);
    dump_map(&handoff->mmap);

    heap_init();
    galloc_init();

    tty_init(&handoff->graphics);

    log_info(ALPHA_ASCII);

    halt();
    __builtin_unreachable();
}
