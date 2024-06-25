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

#include "arch/cmos.h"
#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/panic.h"
#include "arch/pic.h"
#include "drivers/acpi.h"
#include "drivers/ide.h"
#include "drivers/pci.h"
#include "drivers/ps2.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "vfs/fs.h"
#include "video/tty.h"


NORETURN void _kern_entry(boot_handoff* handoff) {
    log_init(&puts);

    if (handoff->magic != BOOT_MAGIC)
        panic("Kernel booted with invalid args!");

    gdt_init();
    idt_init();
    tss_init(handoff->stack_top);

    reclaim_boot_map(&handoff->mmap);

    pmm_init(&handoff->mmap);

    heap_init();
    galloc_init();

    tty_init(&handoff->graphics);

    virtual_fs* vfs = vfs_init();

    pic_init();
    init_ps2_kbd(vfs);

    enable_interrupts();

    log_info(ALPHA_ASCII);

    log_info("Detected %zu MiB of usable RAM", get_total_mem() / MiB);

    std_time time = get_time();
    log_info("Time and date at boot is: %s", asctime(&time));

    acpi_init(handoff->rsdp);

    pci_init();
    dump_pci_devices();

    ide_disk_init(vfs);

    dump_vfs(vfs);

    halt();
    __builtin_unreachable();
}
