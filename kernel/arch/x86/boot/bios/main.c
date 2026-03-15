#include <base/attributes.h>
#include <base/types.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/regs.h>
#include <x86/serial.h>

#include "config.h"
#include "disk.h"
#include "elf.h"
#include "memory.h"
#include "stdlib.h"
#include "tty.h"
#include "vesa.h"

ALIGNED(8)
static boot_info_t info = {0};

NORETURN void _load_entry(u16 boot_disk) {
    init_serial(SERIAL_COM1, SERAIL_DEFAULT_LINE, SERIAL_DEFAULT_BAUD);

    printf("starting apheleiaOS\n\r");

    get_e820(&info.memory_map);
    arch_init_alloc();

    void *smp_trampoline =
        mmap_try_alloc_top(0x1000, E820_KERNEL, 0x1000, 0x000fffffULL);

    if (smp_trampoline) {
        info.smp_trampoline_paddr = (u64)(uintptr_t)smp_trampoline;
    } else {
        printf("no low page for SMP trampoline, using UP mode\n\r");
    }

    u64 acpi_root_ptr = 0;
    get_rsdp(&acpi_root_ptr);
    info.acpi_root_ptr = acpi_root_ptr;

    disk_init(boot_disk);
    bios_boot_root_hint(&info.boot_root_hint);

    printf("parsing config\n\r");
    parse_config(&info.args);

    u64 rootfs_paddr = 0;
    u64 rootfs_size = 0;

    if (info.args.stage_rootfs && stage_rootfs_image(&rootfs_paddr, &rootfs_size)) {
        info.boot_rootfs_paddr = rootfs_paddr;
        info.boot_rootfs_size = rootfs_size;

        // printf(
        //     "staged rootfs paddr=0x%llx size=%llu\n\r",
        //     rootfs_paddr,
        //     rootfs_size
        // );
    }

    printf("initializing video\n\r");
    init_graphics(&info);
    // printf("video mode=%u\n\r", info.video.mode);

    if (info.video.mode == VIDEO_GRAPHICS && info.video.framebuffer) {
        u64 pitch = info.video.bytes_per_line;
        if (!pitch) {
            pitch = (u64)info.video.width * info.video.bytes_per_pixel;
        }

        u64 fb_size = pitch * info.video.height;
        if (fb_size) {
            mmap_add_entry(
                &info.memory_map, info.video.framebuffer, fb_size, E820_RESERVED
            );
            clean_mmap(&info.memory_map);
        }
    }

    printf("jumping to kernel\n\r");

    load_kerenel(&info);

    halt();
    __builtin_unreachable();
}
