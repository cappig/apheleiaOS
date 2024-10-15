#include <alloc/bitmap.h>
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

#include "arch/cmos.h"
#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/panic.h"
#include "arch/pic.h"
#include "arch/stacktrace.h"
#include "drivers/acpi.h"
#include "drivers/console.h"
#include "drivers/ide.h"
#include "drivers/initrd.h"
#include "drivers/pci.h"
#include "drivers/ps2.h"
#include "drivers/serial.h"
#include "drivers/vesa.h"
#include "drivers/zero.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "vfs/fs.h"
#include "video/tty.h"


NORETURN void _kern_entry(boot_handoff* handoff) {
    console_set_serial(SERIAL_COM1);
    log_init(&kputs);

    if (handoff->magic != BOOT_MAGIC)
        panic("Kernel booted with invalid args!");

    init_serial(SERIAL_COM1, handoff->args.serial_baud);

    gdt_init();
    idt_init();
    tss_init(handoff->stack_top);

    reclaim_boot_map(&handoff->mmap);

    pmm_init(&handoff->mmap);

    heap_init();
    galloc_init();

    load_symbols(handoff);

    initrd_init(handoff);

    terminal* tty = tty_init(&handoff->graphics, handoff);

    virtual_fs* vfs = vfs_init();

    init_console(vfs, tty);
    init_serial_dev(vfs);
    init_framebuffer(vfs, &handoff->graphics);
    init_zero_devs(vfs);
    init_ps2_kbd(vfs);

    pic_init();
    enable_interrupts();

    log_info(ALPHA_ASCII);
    log_info(ALPHA_BUILD_DATE);

    dump_gfx_info(&handoff->graphics);

    dump_map(&handoff->mmap);
    log_info("Detected %zu MiB of usable RAM", get_total_mem() / MiB);

    initrd_close(handoff);

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
