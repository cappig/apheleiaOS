#include "elf.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <base/units.h>
#include <log/log.h>
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
#include "vesa.h"

#define BOOT_PAGE_WRITE    (1 << 1)
#define BOOT_PAGE_PAT_HUGE (1ULL << 12)

static bool has_long_mode(void) {
    cpuid_regs_t regs = { 0 };
    cpuid(0x80000000, &regs);

    if (regs.eax < CPUID_EXTENDED_INFO) {
        return false;
    }

    cpuid(CPUID_EXTENDED_INFO, &regs);
    return (regs.edx & CPUID_EI_LM) != 0;
}

static u64 map_top(const e820_map_t *map) {
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

static void commit_log(boot_info_t *info) {
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

static elf_header_t *read_kernel(bool want_64, bool *is_64, size_t *size_out) {
    const char *path = boot_kernel_path(want_64);
    size_t kernel_size = 0;
    elf_header_t *kernel = read_rootfs(path, &kernel_size);

    if (!kernel || kernel_size < sizeof(elf32_header_t)) {
        free(kernel);
        return NULL;
    }

    elf_validity_t validity = elf_verify(kernel);
    if (validity) {
        log_error("invalid kernel ELF %s (error %d)", path, validity);
        free(kernel);
        return NULL;
    }

    if (want_64) {
        if (kernel->machine != EM_X86_64) {
            log_error("kernel arch mismatch at %s: expected x86_64, machine=%#x", path, kernel->machine);
            free(kernel);
            return NULL;
        }

        *is_64 = true;
        *size_out = kernel_size;
        return kernel;
    }

    if (kernel->machine != EM_X86) {
        log_error("kernel arch mismatch at %s: expected x86_32, machine=%#x", path, kernel->machine);
        free(kernel);
        return NULL;
    }

    *is_64 = false;
    *size_out = kernel_size;
    return kernel;
}

void load_kernel(boot_info_t *info) {
    bool is_64 = false;
    size_t kernel_size = 0;
    bool can_load_64 = has_long_mode();
    elf_header_t *kernel = read_kernel(can_load_64, &is_64, &kernel_size);

    if (!kernel && can_load_64) {
        kernel = read_kernel(false, &is_64, &kernel_size);
    }

    if (!kernel) {
        panic("no suitable kernel image found");
    }

    if (!is_64) {
        log_info("loading x86_32 kernel");

        setup_paging_32();

        u32 entry = load_elf_sections_32(kernel, kernel_size);
        if (!entry) {
            panic("failed to load x86_32 kernel ELF");
        }

        u64 mem_top = map_top(&info->memory_map);
        u32 phys_top = (u32)mem_top;

        if (mem_top > 0xffffffffULL) {
            phys_top = 0xffffffffU;
        }

        u32 stack_cap = min(phys_top, (u32)LINEAR_MAP_OFFSET_32);

        identity_map_32(phys_top, 0, false);

        init_paging_32();
        tty_disable_bios_output();

        u32 stack_paddr = (u32)(uintptr_t)mmap_alloc_top(KERNEL_STACK_SIZE, E820_KERNEL, (size_t)(4 * KIB), stack_cap);
        u32 stack = stack_paddr + KERNEL_STACK_SIZE;

        u32 boot_info = (u32)(uintptr_t)info;

        log_debug("kernel entry %#x", (unsigned int)entry);

        commit_log(info);
        jump_to_kernel_32(entry, boot_info, stack);
    } else {
        log_info("loading x86_64 kernel");

        setup_paging_64();

        u64 entry = load_elf_sections_64(kernel, kernel_size);
        if (!entry) {
            panic("failed to load x86_64 kernel ELF");
        }

        identity_map_64(PROTECTED_MODE_TOP, 0, false);
        identity_map_64(PROTECTED_MODE_TOP, LINEAR_MAP_OFFSET_64, true);

        u64 fb_size = video_fb_size(&info->video);
        if (fb_size && info->video.framebuffer < PROTECTED_MODE_TOP &&
            fb_size <= PROTECTED_MODE_TOP - info->video.framebuffer) {
            u64 flags = BOOT_PAGE_WRITE | BOOT_PAGE_PAT_HUGE;

            u64 fb_base = ALIGN_DOWN(info->video.framebuffer, 2 * MIB);
            u64 fb_end = ALIGN(info->video.framebuffer + fb_size, 2 * MIB);
            u64 map_end = fb_end;
            if (map_end > PROTECTED_MODE_TOP) {
                map_end = PROTECTED_MODE_TOP;
            }

            for (u64 a = fb_base; a < map_end; a += 2 * MIB) {
                map_page_64(2 * MIB, a, a, flags, false);
                map_page_64(2 * MIB, a + LINEAR_MAP_OFFSET_64, a, flags, true);
            }
        }

        pat_init();

        init_paging_64();
        tty_disable_bios_output();

        u64 stack_paddr = (u64)(uintptr_t)mmap_alloc(KERNEL_STACK_SIZE, E820_KERNEL, 0);
        u64 stack = stack_paddr + KERNEL_STACK_SIZE + LINEAR_MAP_OFFSET_64;

        u64 boot_info = (uintptr_t)info + LINEAR_MAP_OFFSET_64;

        log_debug("kernel entry %#llx", entry);

        commit_log(info);
        jump_to_kernel_64(entry, boot_info, stack);
    }
}
