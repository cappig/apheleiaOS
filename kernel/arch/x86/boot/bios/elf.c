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

    if (regs.eax < CPUID_EXTENDED_INFO) {
        return false;
    }

    cpuid(CPUID_EXTENDED_INFO, &regs);
    return (regs.edx & CPUID_EI_LM) != 0;
}

static u64 _get_map_top(const e820_map_t *map) {
    u64 top = 0;

    for (size_t i = 0; i < map->count; i++) {
        const e820_entry_t *entry = &map->entries[i];
        if (!entry->size) {
            continue;
        }

        u64 end = entry->address + entry->size;
        if (end > top) {
            top = end;
        }
    }

    return top;
}

static void _commit_boot_log(boot_info_t *info) {
    if (!info) {
        return;
    }

    size_t len = 0;
    size_t cap = 0;
    const char *buf = boot_log_buffer(&len, &cap);

    if (!buf || !cap) {
        return;
    }

    info->boot_log_paddr = (u64)(uintptr_t)buf;
    info->boot_log_len = (u32)len;
    info->boot_log_cap = (u32)cap;
}

static elf_header_t *_load_kernel_image(bool want_64, bool *is_64) {
    const char *path = boot_kernel_path(want_64);
    elf_header_t *kernel = read_rootfs(path);

    if (!kernel) {
        return NULL;
    }

    elf_validity_t validity = elf_verify(kernel);
    if (validity) {
        printf("invalid ELF file %s (error %d)\n\r", path, validity);
        free(kernel);
        return NULL;
    }

    if (want_64) {
        if (kernel->machine != EM_X86_64) {
            printf(
                "expected 64-bit kernel at %s, found machine type 0x%x\n\r",
                path,
                kernel->machine
            );
            free(kernel);
            return NULL;
        }

        *is_64 = true;
        return kernel;
    }

    if (kernel->machine != EM_X86) {
        printf(
            "expected 32-bit kernel at %s, found machine type 0x%x\n\r",
            path,
            kernel->machine
        );
        free(kernel);
        return NULL;
    }

    *is_64 = false;
    return kernel;
}

void load_kerenel(boot_info_t *info) {
    bool want_64 = _cpu_has_long_mode();
    bool is_64 = false;
    elf_header_t *kernel = _load_kernel_image(want_64, &is_64);

    if (!kernel) {
        panic("no suitable kernel image found");
    }

    if (!is_64) {
        printf("loading 32-bit kernel\n\r");

        setup_paging_32();

        u32 entry = load_elf_sections_32(kernel);

        u64 mem_top = _get_map_top(&info->memory_map);
        u32 phys_top = (mem_top > 0xffffffffULL) ? 0xffffffffU : (u32)mem_top;

        identity_map_32(phys_top, 0, false);

        init_paging_32();

        u32 stack_paddr = (u32)(uintptr_t)mmap_alloc_top(
            KERNEL_STACK_SIZE, E820_KERNEL, (size_t)(4 * KIB), phys_top
        );
        u32 stack = stack_paddr + KERNEL_STACK_SIZE;

        u32 boot_info = (u32)(uintptr_t)info;

        serial_printf("jumping to kernel at %#x\n\r", entry);

        _commit_boot_log(info);
        jump_to_kernel_32(entry, boot_info, stack);
    } else {
        printf("loading 64-bit kernel\n\r");

        setup_paging_64();

        u64 entry = load_elf_sections_64(kernel);

        identity_map_64(PROTECTED_MODE_TOP, 0, false);
        identity_map_64(PROTECTED_MODE_TOP, LINEAR_MAP_OFFSET_64, true);

        if (info->video.mode == VIDEO_GRAPHICS && info->video.framebuffer) {
            u64 pitch = info->video.bytes_per_line;
            if (!pitch) {
                pitch = (u64)info->video.width * info->video.bytes_per_pixel;
            }

            u64 fb_size = pitch * info->video.height;
            if (fb_size) {
                // PT_WRITE | PT_PAT_HUGE
                u64 flags = (1 << 1) | (1ULL << 12);

                u64 fb_base = ALIGN_DOWN(info->video.framebuffer, 2*MIB);
                u64 fb_end = ALIGN(info->video.framebuffer + fb_size, 2*MIB);

                for (u64 a = fb_base; a < fb_end && a < PROTECTED_MODE_TOP; a += 2*MIB) {
                    map_page_64(2*MIB, a, a, flags, false);
                    map_page_64(2*MIB, a + LINEAR_MAP_OFFSET_64, a, flags, true);
                }
            }
        }

        pat_init();

        init_paging_64();

        u64 stack_paddr =
            (u64)(uintptr_t)mmap_alloc(KERNEL_STACK_SIZE, E820_KERNEL, 0);
        u64 stack = stack_paddr + KERNEL_STACK_SIZE + LINEAR_MAP_OFFSET_64;

        u64 boot_info = (uintptr_t)info + LINEAR_MAP_OFFSET_64;

        serial_printf("jumping to kernel at %#llx\n\r", entry);

        _commit_boot_log(info);
        jump_to_kernel_64(entry, boot_info, stack);
    }
}
