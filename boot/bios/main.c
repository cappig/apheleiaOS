#include <base/attributes.h>
#include <base/types.h>
#include <boot/proto.h>
#include <parse/elf.h>
#include <x86/asm.h>
#include <x86/serial.h>

#include "disk.h"
#include "handoff.h"
#include "load_elf.h"
#include "memory.h"
#include "paging.h"
#include "tty.h"
#include "vesa.h"

ALIGNED(8)
static boot_handoff handoff = {.magic = BOOT_MAGIC};


NORETURN void _load_entry(u16 boot_disk) {
    init_serial(SERIAL_COM(1), SERIAL_DEFAULT_BAUD);

    cpuid_regs r = {0};
    cpuid(CPUID_EXTENDED_INFO, &r);

    if (!(r.edx & CPUID_EI_LM))
        panic("CPU doesn't support long mode!");

    get_e820(&handoff.mmap);

    handoff.rsdp = get_rsdp();

    init_disk(boot_disk);

    // TODO: read this from a config
    init_graphics(&handoff.graphics, GFX_VESA, 1280, 720, 32);

    setup_paging();

    // Read the kernel elf file
    file_handle kernel_elf = {0};
    open_root_file(&kernel_elf, "kernel.elf");

    if (!kernel_elf.size)
        panic("kernel.elf not found!");

    if (elf_verify(kernel_elf.addr) != VALD_ELF)
        panic("kernel.elf is not a valid executable!");

    u64 kernel_entry = load_elf_sections(&kernel_elf);

    identity_map(PROTECTED_MODE_TOP, 0, false);
    identity_map(PROTECTED_MODE_TOP, ID_MAP_OFFSET, true);

    init_paging();

    handoff.stack_top = alloc_kernel_stack(KERNEL_STACK_SIZE);

    jump_to_kernel(kernel_entry, (u64)ID_MAPPED_VADDR(&handoff), handoff.stack_top);

    halt();
    __builtin_unreachable();
}
