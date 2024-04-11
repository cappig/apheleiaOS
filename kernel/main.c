#include <alloc/bitmap.h>
#include <base/addr.h>
#include <base/attributes.h>
#include <base/types.h>
#include <boot/proto.h>
#include <log/log.h>
#include <x86/asm.h>
#include <x86/paging.h>

#include "mem/physical.h"
#include "video/tty.h"


NORETURN void _kern_entry(boot_handoff* handoff) {
    log_init(&puts);

    if (handoff->magic != BOOT_MAGIC)
        panic("Kernel booted with invalid args!");

    reclaim_boot_map(&handoff->mmap);

    pmm_init(&handoff->mmap);
    // dump_map(&handoff->mmap);

    halt();
    __builtin_unreachable();
}
