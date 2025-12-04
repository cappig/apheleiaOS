#include "elf.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <parse/elf.h>
#include <stdint.h>
#include <stdio.h>
#include <x86/boot.h>

#include "disk.h"
#include "memory.h"
#include "paging32.h"
#include "paging64.h"
#include "tty.h"


extern void jump_to_kernel_64(u64 entry, u64 boot_info, u64 stack);

void load_kerenel(boot_info_t* info) {
    elf_header_t* kernel = read_rootfs("/boot/kernel.elf");

    if (!kernel)
        panic("/boot/kernel.elf not found!");

    if (elf_verify(kernel))
        panic("/boot/kernel.elf is not valid!");

    if (kernel->machine == EM_X86) {
        printf("Loading 32 bit kernel...\n\r");

        setup_paging_32();

        // u32 addr = load_elf_sections_32(kernel);

        panic("TODO: IMPLEMENT 32 BIT KERNEL LOADING");
    } else if (kernel->machine == EM_X86_64) {
        printf("Loading 64 bit kernel...\n\r");

        setup_paging_64();

        u64 entry = load_elf_sections_64(kernel);

        identity_map_64(PROTECTED_MODE_TOP, 0, false);
        identity_map_64(PROTECTED_MODE_TOP, LINEAR_MAP_OFFSET_64, true);

        init_paging_64();

        u64 stack_paddr = (u64)(uintptr_t)mmap_alloc(KERNEL_STACK_SIZE, E820_KERNEL, 0);
        u64 stack = stack_paddr + LINEAR_MAP_OFFSET_64;

        u64 boot_info = (uintptr_t)info + LINEAR_MAP_OFFSET_64;

        printf("Jumping to kernel at %#llx\n\r", entry);

        jump_to_kernel_64(entry, boot_info, stack);
    } else {
        panic("/boot/kernel.elf has invalid arch!");
    }
}
