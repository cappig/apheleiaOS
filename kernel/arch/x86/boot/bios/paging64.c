#include "paging64.h"

#include <base/macros.h>
#include <base/types.h>
#include <common/elf.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "memory.h"
#include "parse/elf.h"
#include "x86/asm.h"
#include "x86/paging64.h"

typedef struct {
    page_t *lvl4;
    bool nx;
} paging64_state_t;

static paging64_state_t paging64;

static u32 table_type(bool is_kernel) {
    if (is_kernel) {
        return E820_KERNEL;
    }

    return E820_PAGE_TABLE;
}

static bool has_nx(void) {
    cpuid_regs_t regs = { 0 };
    cpuid(0x80000000, &regs);

    if (regs.eax < CPUID_EXTENDED_INFO) {
        return false;
    }

    cpuid(CPUID_EXTENDED_INFO, &regs);
    return (regs.edx & CPUID_EI_NX) != 0;
}

static page_t *walk_table(page_t *table, size_t index, bool is_kernel) {
    page_t *next_table;

    if (table[index] & PT_PRESENT) {
        next_table = (page_t *)(uintptr_t)page_get_paddr(&table[index]);
    } else {
        u32 type = table_type(is_kernel);

        next_table = (page_t *)mmap_alloc(PAGE_4KIB, type, PAGE_4KIB);
        memset(next_table, 0, PAGE_4KIB);

        page_set_paddr(&table[index], (u64)(uintptr_t)next_table);

        table[index] |= PT_PRESENT;
        table[index] |= PT_WRITE;
    }

    return next_table;
}

// huge pages are used only when the caller has already aligned the range
void map_page_64(size_t size, u64 vaddr, u64 paddr, u64 flags, bool is_kernel) {
    size_t lvl4_index = GET_LVL4_INDEX(vaddr);

    size_t lvl3_index = GET_LVL3_INDEX(vaddr);
    page_t *lvl3 = walk_table(paging64.lvl4, lvl4_index, is_kernel);

    page_t *entry;

    if (size == PAGE_1GIB) {
        entry = &lvl3[lvl3_index];

        paddr = ALIGN_DOWN(paddr, PAGE_1GIB);
        flags |= PT_HUGE;
        goto finalize;
    }

    size_t lvl2_index = GET_LVL2_INDEX(vaddr);
    page_t *lvl2 = walk_table(lvl3, lvl3_index, is_kernel);

    if (size == PAGE_2MIB) {
        entry = &lvl2[lvl2_index];

        paddr = ALIGN_DOWN(paddr, PAGE_2MIB);
        flags |= PT_HUGE;
        goto finalize;
    }

    size_t lvl1_index = GET_LVL1_INDEX(vaddr);
    page_t *lvl1 = walk_table(lvl2, lvl2_index, is_kernel);

    entry = &lvl1[lvl1_index];

finalize:
    page_set_paddr(entry, paddr);

    flags |= PT_PRESENT;
    u64 pat_huge = 0;

    if (flags & PT_HUGE) {
        pat_huge = flags & PT_PAT_HUGE;
    }

    *entry |= (flags & FLAGS_MASK) | pat_huge;
}

void map_region_64(size_t size, u64 vaddr, u64 paddr, u64 flags, bool is_kernel) {
    size_t remaining = ALIGN(size, PAGE_4KIB);

    while (remaining) {
        size_t page_size = PAGE_4KIB;

        if (remaining >= PAGE_2MIB && (vaddr & (PAGE_2MIB - 1)) == 0 && (paddr & (PAGE_2MIB - 1)) == 0) {
            page_size = PAGE_2MIB;
        }

        map_page_64(page_size, vaddr, paddr, flags, is_kernel);

        vaddr += page_size;
        paddr += page_size;
        remaining -= page_size;
    }
}

void identity_map_64(u64 top_address, u64 offset, bool is_kernel) {
    for (u64 i = 0; i < top_address; i += PAGE_2MIB) {
        map_page_64(PAGE_2MIB, i + offset, i, PT_WRITE, is_kernel);
    }
}

void setup_paging_64(void) {
    paging64.lvl4 = (page_t *)mmap_alloc(PAGE_4KIB, E820_KERNEL, PAGE_4KIB);
    memset(paging64.lvl4, 0, PAGE_4KIB);
    write_cr3((u32)(uintptr_t)paging64.lvl4);

    paging64.nx = has_nx();

    // NX is optional on older machines, so the bootloader enables it only if CPUID says so
    if (paging64.nx) {
        u64 efer = read_msr(EFER_MSR);
        write_msr(EFER_MSR, efer | EFER_NX);
    }

    // write protect keeps supervisor writes honest once readonly kernel pages exist
    u32 cr0 = read_cr0();
    write_cr0(cr0 | CR0_WP);
}

void init_paging_64(void) {
    u32 cr4 = read_cr4();
    write_cr4(cr4 | CR4_PAE);

    u64 efer = read_msr(EFER_MSR);
    write_msr(EFER_MSR, efer | EFER_LME);

    u32 cr0 = read_cr0();
    write_cr0(cr0 | CR0_PG);
}

static page_t elf_flags(u32 elf_flags) {
    u64 flags = PT_PRESENT;

    if (elf_flags & PF_W) {
        flags |= PT_WRITE;
    }

    if (paging64.nx && !(elf_flags & PF_X)) {
        flags |= PT_NO_EXECUTE;
    }

    return flags;
}

struct elf_load_ctx64 {
    void *elf;
};

static bool load_segment_64(const elf_segment_t *seg, void *ctx_ptr) {
    struct elf_load_ctx64 *load_ctx = ctx_ptr;

    u64 size = ALIGN(seg->mem_size, PAGE_4KIB);
    u64 flags = elf_flags(seg->flags);

    u64 pbase = (u64)(uintptr_t)mmap_alloc(size, E820_KERNEL, (size_t)seg->align);
    u64 vbase = seg->vaddr;

    map_region_64(size, vbase, pbase, flags, true);

    memcpy((void *)(uintptr_t)pbase, (u8 *)load_ctx->elf + seg->offset, (size_t)seg->file_size);

    size_t zero_len = (size_t)(seg->mem_size - seg->file_size);
    memset((u8 *)(uintptr_t)pbase + seg->file_size, 0, zero_len);

    return true;
}

u64 load_elf_sections_64(void *elf_file) {
    struct elf_load_ctx64 ctx = {
        .elf = elf_file,
    };
    elf_info_t info = { 0 };

    if (!elf_foreach_segment(elf_file, 0, load_segment_64, &ctx, &info)) {
        return 0;
    }

    if (!info.is_64) {
        return 0;
    }

    return info.entry;
}
