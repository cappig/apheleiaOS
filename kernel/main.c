#include <alloc/global.h>
#include <base/addr.h>
#include <base/attributes.h>
#include <base/types.h>
#include <boot/proto.h>
#include <gfx/state.h>
#include <log/log.h>
#include <x86/asm.h>
#include <x86/e820.h>
#include <x86/paging.h>
#include <x86/serial.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/irq.h"
#include "arch/pic.h"
#include "arch/tsc.h"
#include "drivers/acpi.h"
#include "drivers/ide.h"
#include "drivers/initrd.h"
#include "drivers/iso9660.h"
#include "drivers/pci.h"
#include "drivers/ps2.h"
#include "drivers/serial.h"
#include "drivers/vesa.h"
#include "drivers/zero.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "sched/scheduler.h"
#include "sys/clock.h"
#include "sys/console.h"
#include "sys/cpu.h"
#include "sys/panic.h"
#include "sys/symbols.h"
#include "sys/tty.h"
#include "sys/video.h"
#include "vfs/fs.h"


NORETURN void _kern_entry(boot_handoff* handoff) {
    log_init(&kputs);

    if (handoff->magic != BOOT_MAGIC)
        panic("Kernel booted with invalid args!");

    if (handoff->args.debug == DEBUG_NONE)
        log_set_lvl(LOG_INFO);

    cpu_set_gs_base((u64)&cores_local[0]);
    cpu_init_core(0);

    gdt_init();
    pic_init();
    idt_init();
    tss_init(handoff->stack_top);

    reclaim_boot_map(&handoff->mmap);
    pmm_init(&handoff->mmap);

    heap_init();
    galloc_init();

    video_init(&handoff->graphics);

    clock_init();
    calibrate_tsc();

    acpi_init(handoff->rsdp);

    pci_init();

    irq_init();

    vfs_init();

    ustar_init();
    initrd_mount(handoff);

    init_serial_dev();

    tty_init(handoff);
    tty_spawn_devs();

    iso_init();

    load_symbols();

    print_motd(handoff);

    scheduler_init();

    init_framebuffer_dev(handoff);
    init_zero_devs();
    init_ps2();

    ide_disk_init();

    // dump_vfs();

    enable_interrupts();

    scheduler_start();

    halt();
    __builtin_unreachable();
}
