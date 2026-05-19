#include "paging32.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "memory.h"
#include "parse/elf.h"
#include "x86/asm.h"
#include "x86/paging32.h"

#include <common/elf.h>

static page_t *pdpt;

static page_t *_walk_pdpt_once(size_t index, bool is_kernel) {
    page_t *pd;

    if (pdpt[index] & PT_PRESENT) {
        pd = (page_t *)(uintptr_t)page_get_paddr(&pdpt[index]);
    } else {
        u32 type = is_kernel ? E820_KERNEL : E820_PAGE_TABLE;
        pd = (page_t *)mmap_alloc_top(
            PAGE_4KIB, type, PAGE_4KIB, LINEAR_MAP_OFFSET_32
        );
        memset(pd, 0, PAGE_4KIB);

        page_set_paddr(&pdpt[index], (page_t)(uintptr_t)pd);
        pdpt[index] |= PT_PRESENT;
    }

    return pd;
}

static page_t *_walk_pd_once(page_t *pd, size_t index, bool is_kernel) {
    page_t *pt;

    if (pd[index] & PT_PRESENT) {
        pt = (page_t *)(uintptr_t)page_get_paddr(&pd[index]);
    } else {
        u32 type = is_kernel ? E820_KERNEL : E820_PAGE_TABLE;
        pt = (page_t *)mmap_alloc_top(
            PAGE_4KIB, type, PAGE_4KIB, LINEAR_MAP_OFFSET_32
        );
        memset(pt, 0, PAGE_4KIB);

        page_set_paddr(&pd[index], (page_t)(uintptr_t)pt);
        pd[index] |= PT_PRESENT;
        pd[index] |= PT_WRITE;
    }

    return pt;
}

static void reserve_phys_window_32(void) {
    u32 base = PHYS_WINDOW_BASE_32;
    u32 size = PHYS_WINDOW_SIZE_32;

    for (u32 offset = 0; offset < size; offset += PAGE_2MIB) {
        u32 vaddr = base + offset;

        size_t pdpt_index = GET_LVL3_INDEX(vaddr);
        size_t pd_index = GET_LVL2_INDEX(vaddr);

        page_t *pd = _walk_pdpt_once(pdpt_index, false);

        if (pd[pd_index] & PT_PRESENT) {
            continue;
        }

        page_t *pt = (page_t *)mmap_alloc_top(
            PAGE_4KIB, E820_PAGE_TABLE, PAGE_4KIB, LINEAR_MAP_OFFSET_32
        );
        memset(pt, 0, PAGE_4KIB);

        page_set_paddr(&pd[pd_index], (page_t)(uintptr_t)pt);
        pd[pd_index] |= PT_PRESENT;
        pd[pd_index] |= PT_WRITE;
    }
}

// size must be either PAGE_4KIB or PAGE_2MIB
void map_page_32(size_t size, u32 vaddr, u64 paddr, u64 flags, bool is_kernel) {
    size_t lvl3_index = GET_LVL3_INDEX(vaddr);
    size_t lvl2_index = GET_LVL2_INDEX(vaddr);
    size_t lvl1_index = GET_LVL1_INDEX(vaddr);

    page_t *pd = _walk_pdpt_once(lvl3_index, is_kernel);

    if (size == PAGE_2MIB) {
        page_t *dir_entry = &pd[lvl2_index];

        paddr = ALIGN_DOWN(paddr, PAGE_2MIB);
        page_set_paddr(dir_entry, (page_t)paddr);

        flags |= PT_HUGE;
        flags |= PT_PRESENT;
        *dir_entry |= flags & FLAGS_MASK;
        return;
    }

    // otherwise, ensure page table exists and set 4 KiB page entry
    page_t *pt = _walk_pd_once(pd, lvl2_index, is_kernel);

    page_t *entry = &pt[lvl1_index];

    page_set_paddr(entry, (page_t)paddr);

    flags |= PT_PRESENT;
    *entry |= flags & FLAGS_MASK;
}

// Map region by breaking into 4 KiB pages
void map_region_32(
    size_t size,
    u32 vaddr,
    u64 paddr,
    u64 flags,
    bool is_kernel
) {
    for (size_t i = 0; i < DIV_ROUND_UP(size, PAGE_4KIB); i++) {
        u32 page_vaddr = vaddr + (u32)(i * PAGE_4KIB);
        u64 page_paddr = paddr + (u64)(i * PAGE_4KIB);

        map_page_32(PAGE_4KIB, page_vaddr, page_paddr, flags, is_kernel);
    }
}

// Identity-map physical memory up to top_address
// Use 2 MiB pages where possible
void identity_map_32(u32 top_address, u32 offset, bool is_kernel) {
    u32 map_top = top_address;

    if (map_top > LINEAR_MAP_OFFSET_32) {
        map_top = LINEAR_MAP_OFFSET_32;
    }

    for (u32 i = 0; i < map_top; i += PAGE_2MIB) {
        map_page_32(PAGE_2MIB, i + offset, (u64)i, PT_WRITE, is_kernel);
    }
}

void setup_paging_32(void) {
    // Allocate the PDPT (root)
    pdpt = (page_t *)mmap_alloc_top(
        PAGE_4KIB, E820_KERNEL, PAGE_4KIB, LINEAR_MAP_OFFSET_32
    );
    memset(pdpt, 0, PAGE_4KIB);

    // write physical base into CR3
    write_cr3((u32)(uintptr_t)pdpt);

    reserve_phys_window_32();
}

void init_paging_32(void) {
    // Enable PAE
    u32 cr4 = read_cr4();
    write_cr4(cr4 | CR4_PAE | CR4_PSE);

    // Enable paging by setting CR0.PG
    u32 cr0 = read_cr0();
    write_cr0(cr0 | CR0_PG);
}

static page_t _elf_to_page_flags(u32 elf_flags) {
    u64 flags = PT_PRESENT;

    if (elf_flags & PF_W) {
        flags |= PT_WRITE;
    }

    return flags;
}

struct elf_load_ctx32 {
    void *elf;
};

static bool load_segment_32(const elf_segment_t *seg, void *ctx_ptr) {
    struct elf_load_ctx32 *load_ctx = ctx_ptr;

    u32 size = ALIGN((u32)seg->mem_size, PAGE_4KIB);
    u64 flags = _elf_to_page_flags(seg->flags);

    u64 pbase =
        (u64)(uintptr_t)mmap_alloc(size, E820_KERNEL, (size_t)seg->align);
    u32 vbase = (u32)seg->vaddr;

    // Map the segment
    map_region_32(size, vbase, pbase, flags, true);

    // Copy all loadable data from the file
    memcpy(
        (void *)(uintptr_t)pbase,
        (u8 *)load_ctx->elf + seg->offset,
        (size_t)seg->file_size
    );

    // Zero out any additional space
    size_t zero_len = (size_t)(seg->mem_size - seg->file_size);
    memset((void *)(uintptr_t)pbase + seg->file_size, 0, zero_len);

    return true;
}

u32 load_elf_sections_32(void *elf_file) {
    struct elf_load_ctx32 ctx = {
        .elf = elf_file,
    };

    elf_info_t info = {0};
    if (!elf_foreach_segment(elf_file, 0, load_segment_32, &ctx, &info)) {
        return 0;
    }

    if (info.is_64) {
        return 0;
    }

    return (u32)info.entry;
}
