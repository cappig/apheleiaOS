#include "elf.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <base/units.h>
#include <parse/elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <x86/asm.h>
#include <x86/boot.h>

#include "disk.h"
#include "memory.h"
#include "paging32.h"
#include "paging64.h"
#include "tty.h"

static bool _cpu_has_long_mode(void) {
    cpuid_regs_t regs = {0};
    cpuid(0x80000000, &regs);

    if (regs.eax < CPUID_EXTENDED_INFO)
        return false;

    cpuid(CPUID_EXTENDED_INFO, &regs);
    return (regs.edx & CPUID_EI_LM) != 0;
}

static u64 _get_usable_top(const e820_map_t* map) {
    u64 top = 0;

    for (size_t i = 0; i < map->count; i++) {
        const e820_entry_t* entry = &map->entries[i];

        if (entry->type != E820_AVAILABLE)
            continue;

        u64 end = entry->address + entry->size;
        if (end > top)
            top = end;
    }

    return top;
}

static elf_header_t* _load_kernel_image(bool want_64, bool* is_64) {
    const char* paths64[] = {"/boot/kernel64.elf", "/boot/kernel32.elf", "/boot/kernel.elf"};
    const char* paths32[] = {"/boot/kernel32.elf", "/boot/kernel.elf", "/boot/kernel64.elf"};
    const char** paths = want_64 ? paths64 : paths32;

    for (size_t i = 0; i < ARRAY_LEN(paths64); i++) {
        elf_header_t* kernel = read_rootfs(paths[i]);

        if (!kernel)
            continue;

        if (elf_verify(kernel)) {
            free(kernel);
            continue;
        }

        if (kernel->machine == EM_X86_64) {
            if (!want_64) {
                free(kernel);
                continue;
            }

            *is_64 = true;
            return kernel;
        }

        if (kernel->machine == EM_X86) {
            *is_64 = false;
            return kernel;
        }

        free(kernel);
    }

    return NULL;
}

void load_kerenel(boot_info_t* info) {
    bool want_64 = _cpu_has_long_mode();
    bool is_64 = false;
    elf_header_t* kernel = _load_kernel_image(want_64, &is_64);

    if (!kernel)
        panic("No suitable kernel image found!");

    if (!is_64) {
        printf("Loading 32 bit kernel...\n\r");

        setup_paging_32();

        u32 entry = load_elf_sections_32(kernel);

        u64 mem_top = _get_usable_top(&info->memory_map);
        u32 phys_top = (mem_top > 0xffffffffULL) ? 0xffffffffU : (u32)mem_top;

        identity_map_32(phys_top, 0, false);

        init_paging_32();

        u32 stack_paddr = (u32)(uintptr_t)mmap_alloc_top(
            KERNEL_STACK_SIZE, E820_KERNEL, (size_t)(4 * KIB), phys_top
        );
        u32 stack = stack_paddr + KERNEL_STACK_SIZE;

        u32 boot_info = (u32)(uintptr_t)info;

        serial_printf("Jumping to kernel at %#x\n\r", entry);

        jump_to_kernel_32(entry, boot_info, stack);
    } else {
        printf("Loading 64 bit kernel...\n\r");

        setup_paging_64();

        u64 entry = load_elf_sections_64(kernel);

        identity_map_64(PROTECTED_MODE_TOP, 0, false);
        identity_map_64(PROTECTED_MODE_TOP, LINEAR_MAP_OFFSET_64, true);

        init_paging_64();

        u64 stack_paddr = (u64)(uintptr_t)mmap_alloc(KERNEL_STACK_SIZE, E820_KERNEL, 0);
        u64 stack = stack_paddr + KERNEL_STACK_SIZE + LINEAR_MAP_OFFSET_64;

        u64 boot_info = (uintptr_t)info + LINEAR_MAP_OFFSET_64;

        serial_printf("Jumping to kernel at %#llx\n\r", entry);

        jump_to_kernel_64(entry, boot_info, stack);
    }
}
