#include <base/attributes.h>
#include <base/types.h>
#include <boot/proto.h>
#include <parse/elf.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/serial.h>

#include "config.h"
#include "disk.h"
#include "handoff.h"
#include "load_elf.h"
#include "memory.h"
#include "paging.h"
#include "tty.h"
#include "vesa.h"

ALIGNED(8)
static boot_handoff handoff = {.magic = BOOT_MAGIC};


static void load_config(void) {
    file_handle args_cfg = {0};
    open_root_file(&args_cfg, "args.cfg");

    parse_config(&args_cfg, &handoff.args);

    close_root_file(&args_cfg);
}

static void load_initrd(void) {
    file_handle initrd = {0};
    open_root_file(&initrd, "initrd.tar");

    if (!initrd.size)
        panic("initrd.tar not found");

    void* initrd_perm = mmap_alloc(initrd.size, E820_KERNEL, 512);
    memcpy(initrd_perm, initrd.addr, initrd.size);

    handoff.initrd_loc = (u32)(uptr)initrd_perm;
    handoff.initrd_size = initrd.size;

    close_root_file(&initrd);
}

static u64 load_kernel(void) {
    file_handle kernel_elf = {0};
    open_root_file(&kernel_elf, "kernel.elf");

    if (!kernel_elf.size)
        panic("kernel.elf not found!");

    if (elf_verify(kernel_elf.addr) != VALD_ELF)
        panic("kernel.elf is not a valid executable!");

    u64 kernel_entry = load_elf_sections(&kernel_elf);

    close_root_file(&kernel_elf);

    return kernel_entry;
}

static void load_symbols(void) {
    file_handle sym_map = {0};
    open_root_file(&sym_map, "sym.map");

    if (!sym_map.size)
        return;

    void* sym_perm = mmap_alloc(sym_map.size, E820_KERNEL, 1);
    memcpy(sym_perm, sym_map.addr, sym_map.size);

    handoff.symtab_loc = (u32)(uptr)sym_perm;
    handoff.symtab_size = sym_map.size;

    close_root_file(&sym_map);
}


NORETURN void _load_entry(u16 boot_disk) {
    init_serial(SERIAL_COM1, SERIAL_DEFAULT_BAUD);

    cpuid_regs r = {0};
    cpuid(CPUID_EXTENDED_INFO, &r);

    if (!(r.edx & CPUID_EI_LM))
        panic("CPU doesn't support long mode!");

    get_e820(&handoff.mmap);

    handoff.rsdp = get_rsdp();

    init_disk(boot_disk);

    load_config();

    init_graphics(
        &handoff.graphics,
        handoff.args.gfx_mode,
        handoff.args.vesa_width,
        handoff.args.vesa_height,
        handoff.args.vesa_bpp
    );

    setup_paging();

    load_initrd();

    u64 kernel_entry = load_kernel();

    load_symbols();

    identity_map(PROTECTED_MODE_TOP, 0, false);
    identity_map(PROTECTED_MODE_TOP, ID_MAP_OFFSET, true);

    init_paging();

    handoff.stack_top = alloc_kernel_stack(KERNEL_STACK_SIZE);

    jump_to_kernel(kernel_entry, (u64)ID_MAPPED_VADDR(&handoff), handoff.stack_top);

    halt();
    __builtin_unreachable();
}
