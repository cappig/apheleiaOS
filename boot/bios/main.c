#include <base/attributes.h>
#include <base/types.h>
#include <boot/proto.h>
#include <x86/asm.h>
#include <x86/serial.h>

#include "disk.h"
#include "memory.h"
#include "paging.h"
#include "tty.h"
#include "vesa.h"

static boot_handoff handoff = {.magic = BOOT_MAGIC};


NORETURN void _load_entry(u16 boot_disk) {
    init_serial(SERIAL_DEFAULT_BAUD);

    get_e820(&handoff.mmap);

    init_disk(boot_disk);

    // TODO: read this from a config
    init_graphics(&handoff.graphics, GFX_VESA, 1280, 720, 32);

    // Read the kernel elf file
    file_handle kernel_elf = {0};
    open_root_file(&kernel_elf, "kernel.elf");

    if (!kernel_elf.size)
        panic("kernel.elf not found!");

    setup_paging();

    identity_map(PROTECTED_MODE_TOP, 0, false);
    identity_map(PROTECTED_MODE_TOP, IDENTITY_MAP_OFFSET, true);

    // init_paging();

    panic("Well shit");

    halt();
    __builtin_unreachable();
}
