#include <arch/arch.h>
#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/lock.h>
#include <sys/panic.h>
#include <x86/asm.h>
#include <x86/boot.h>

#if defined(__x86_64__)
#include "virtual.h"

#define PHYS_WINDOW_BASE_64 0xfffffe0000000000ULL
#define PHYS_WINDOW_SIZE_64 (16 * MIB)
#define PHYS_WINDOW_STACK_MAX 8

typedef struct {
    u64 paddr_base;
    size_t pages;
    u64 flags;
} window_map_t;

static size_t window_pages_mapped = 0;
static u64 window_paddr_base = 0;
static u64 window_flags = PT_WRITE;
static spinlock_t window_lock = SPINLOCK_INIT;
static u32 window_lock_depth[MAX_CORES] = {0};
static unsigned long window_irq_flags[MAX_CORES] = {0};

static window_map_t window_stack[PHYS_WINDOW_STACK_MAX];
static size_t window_stack_depth = 0;

static size_t _window_cpu_id(void) {
    cpu_core_t *core = cpu_current();

    if (!core || core->id >= MAX_CORES) {
        return 0;
    }

    return core->id;
}

static void _window_lock_acquire(void) {
    size_t cpu_id = _window_cpu_id();

    if (window_lock_depth[cpu_id]) {
        window_lock_depth[cpu_id]++;
        return;
    }

    unsigned long irq_flags = arch_irq_save();
    spin_lock(&window_lock);

    window_lock_depth[cpu_id] = 1;
    window_irq_flags[cpu_id] = irq_flags;
}

static void _window_lock_release(void) {
    size_t cpu_id = _window_cpu_id();

    if (!window_lock_depth[cpu_id]) {
        return;
    }

    window_lock_depth[cpu_id]--;

    if (!window_lock_depth[cpu_id]) {
        spin_unlock(&window_lock);
        arch_irq_restore(window_irq_flags[cpu_id]);
    }
}


static u64 _map_flags_to_pt_flags(u32 flags) {
    u64 pt_flags = PT_WRITE;

    if (flags & PHYS_MAP_WC) {
        pt_flags |= PT_PAT_4K;
    }

    if (flags & (PHYS_MAP_UC | PHYS_MAP_MMIO)) {
        pt_flags &= ~PT_PAT_4K;
        pt_flags |= PT_NO_CACHE | PT_WRITE_THROUGH;
    }

    return pt_flags;
}

static page_t *_get_root(void) {
    u64 cr3 = read_cr3();
    return (page_t *)(uintptr_t)(cr3 & ADDR_MASK);
}

static void _map_window_page(u64 vaddr, u64 paddr, u64 flags) {
    map_page(_get_root(), PAGE_4KIB, vaddr, paddr, flags);
}

static void _clear_window_page(u64 vaddr) {
    unmap_page(_get_root(), vaddr);
}

static void _clear_window_range(size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        _clear_window_page(PHYS_WINDOW_BASE_64 + i * PAGE_4KIB);
    }
}

static void _restore_window(window_map_t prev) {
    window_pages_mapped = prev.pages;
    window_paddr_base = prev.paddr_base;
    window_flags = prev.flags;

    for (size_t i = 0; i < prev.pages; i++) {
        _map_window_page(
            PHYS_WINDOW_BASE_64 + i * PAGE_4KIB,
            prev.paddr_base + i * PAGE_4KIB,
            prev.flags
        );
    }
}

void *arch_phys_map(u64 paddr, size_t size, u32 flags) {
    if (!size) {
        return NULL;
    }

    u64 end = paddr + size;
    if (
        !(flags & (PHYS_MAP_UC | PHYS_MAP_MMIO)) &&
        end >= paddr &&
        end <= PROTECTED_MODE_TOP
    ) {
        return (void *)(uintptr_t)(paddr + LINEAR_MAP_OFFSET_64);
    }

    u64 start = ALIGN_DOWN(paddr, PAGE_4KIB);
    end = ALIGN(end, PAGE_4KIB);

    size_t pages = (size_t)((end - start) / PAGE_4KIB);
    size_t window_pages = PHYS_WINDOW_SIZE_64 / PAGE_4KIB;

    if (pages > window_pages) {
        panic("phys window map too large");
    }

    _window_lock_acquire();

    if (window_pages_mapped) {
        if (window_stack_depth >= PHYS_WINDOW_STACK_MAX) {
            panic("phys window map stack overflow");
        }

        window_stack[window_stack_depth++] = (window_map_t){
            .paddr_base = window_paddr_base,
            .pages = window_pages_mapped,
            .flags = window_flags,
        };

        _clear_window_range(window_pages_mapped);
    }

    window_pages_mapped = pages;
    window_paddr_base = start;

    u64 pt_flags = _map_flags_to_pt_flags(flags);

    window_flags = pt_flags;

    for (size_t i = 0; i < pages; i++) {
        _map_window_page(
            PHYS_WINDOW_BASE_64 + i * PAGE_4KIB,
            start + i * PAGE_4KIB,
            pt_flags
        );
    }

    return (void *)(uintptr_t)(PHYS_WINDOW_BASE_64 + (paddr - start));
}

void arch_phys_unmap(void *vaddr, size_t size) {
    (void)size;

    if (!vaddr) {
        return;
    }

    u64 vaddr_u64 = (u64)(uintptr_t)vaddr;
    u64 window_start = PHYS_WINDOW_BASE_64;
    u64 window_end = window_start + PHYS_WINDOW_SIZE_64;

    if (vaddr_u64 < window_start || vaddr_u64 >= window_end) {
        return;
    }

    if (!window_pages_mapped) {
        return;
    }

    _clear_window_range(window_pages_mapped);
    window_pages_mapped = 0;
    window_paddr_base = 0;
    window_flags = PT_WRITE;

    if (!window_stack_depth) {
        _window_lock_release();
        return;
    }

    window_map_t prev = window_stack[--window_stack_depth];
    _restore_window(prev);
    _window_lock_release();
}

bool arch_phys_copy(u64 dst_paddr, u64 src_paddr, size_t size) {
    if (!size) {
        return true;
    }

    static const size_t kChunk = 256;
    u8 bounce[kChunk];

    size_t offset = 0;
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > kChunk) {
            chunk = kChunk;
        }

        // The phys window is a single sliding mapping. Never hold two mapped
        // windows at once or src/dst can alias and silently corrupt copies.
        void *src = arch_phys_map(src_paddr + offset, chunk, 0);
        if (!src) {
            return false;
        }
        memcpy(bounce, src, chunk);
        arch_phys_unmap(src, chunk);

        void *dst = arch_phys_map(dst_paddr + offset, chunk, 0);
        if (!dst) {
            return false;
        }
        memcpy(dst, bounce, chunk);
        arch_phys_unmap(dst, chunk);

        offset += chunk;
    }

    return true;
}
#else
#include <x86/paging32.h>

static size_t window_pages_mapped = 0;
static u64 window_paddr_base = 0;
static u64 window_flags = PT_WRITE;
static spinlock_t window_lock = SPINLOCK_INIT;
static u32 window_lock_depth[MAX_CORES] = {0};
static unsigned long window_irq_flags[MAX_CORES] = {0};

#define PHYS_WINDOW_STACK_MAX 8

typedef struct {
    u64 paddr_base;
    size_t pages;
    u64 flags;
} window_map_t;

static window_map_t window_stack[PHYS_WINDOW_STACK_MAX];
static size_t window_stack_depth = 0;

static size_t _window_cpu_id(void) {
    cpu_core_t *core = cpu_current();

    if (!core || core->id >= MAX_CORES) {
        return 0;
    }

    return core->id;
}

static void _window_lock_acquire(void) {
    size_t cpu_id = _window_cpu_id();

    if (window_lock_depth[cpu_id]) {
        window_lock_depth[cpu_id]++;
        return;
    }

    unsigned long irq_flags = arch_irq_save();
    spin_lock(&window_lock);

    window_lock_depth[cpu_id] = 1;
    window_irq_flags[cpu_id] = irq_flags;
}

static void _window_lock_release(void) {
    size_t cpu_id = _window_cpu_id();

    if (!window_lock_depth[cpu_id]) {
        return;
    }

    window_lock_depth[cpu_id]--;

    if (!window_lock_depth[cpu_id]) {
        spin_unlock(&window_lock);
        arch_irq_restore(window_irq_flags[cpu_id]);
    }
}

static u64 _map_flags_to_pt_flags(u32 flags) {
    u64 pt_flags = PT_WRITE;

    if (flags & PHYS_MAP_WC) {
        pt_flags |= PT_PAT_4K;
    }

    if (flags & (PHYS_MAP_UC | PHYS_MAP_MMIO)) {
        pt_flags &= ~PT_PAT_4K;
        pt_flags |= PT_NO_CACHE | PT_WRITE_THROUGH;
    }

    return pt_flags;
}

static page_t *_get_pdpt(void) {
    u64 cr3 = read_cr3();
    return (page_t *)(uintptr_t)(cr3 & ~0x1fULL);
}

static page_t *_get_window_pt(u32 vaddr) {
    page_t *pdpt = _get_pdpt();
    size_t pdpt_index = GET_LVL3_INDEX(vaddr);

    if (!(pdpt[pdpt_index] & PT_PRESENT))
        panic("PAE PDPT entry missing for phys window");

    page_t *pd = (page_t *)(uintptr_t)page_get_paddr(&pdpt[pdpt_index]);
    size_t pd_index = GET_LVL2_INDEX(vaddr);
    page_t pde = pd[pd_index];

    if (!(pde & PT_PRESENT) || (pde & PT_HUGE))
        panic("PAE PDE missing for phys window");

    return (page_t *)(uintptr_t)page_get_paddr(&pd[pd_index]);
}

static void _map_window_page(u32 vaddr, u64 paddr, u64 flags) {
    page_t *pt = _get_window_pt(vaddr);
    size_t pt_index = GET_LVL1_INDEX(vaddr);
    page_t *entry = &pt[pt_index];

    *entry = 0;
    page_set_paddr(entry, paddr);
    *entry |= (flags | PT_PRESENT) & FLAGS_MASK;

    tlb_flush(vaddr);
}

static void _clear_window_page(u32 vaddr) {
    page_t *pt = _get_window_pt(vaddr);
    size_t pt_index = GET_LVL1_INDEX(vaddr);

    pt[pt_index] = 0;
    tlb_flush(vaddr);
}

static void _clear_window_range(size_t pages) {
    u32 vaddr = PHYS_WINDOW_BASE_32;

    for (size_t i = 0; i < pages; i++)
        _clear_window_page(vaddr + (u32)(i * PAGE_4KIB));
}

void *arch_phys_map(u64 paddr, size_t size, u32 flags) {
    if (!size)
        return NULL;

    u64 start = ALIGN_DOWN(paddr, PAGE_4KIB);
    u64 end = ALIGN(paddr + size, PAGE_4KIB);
    size_t pages = (size_t)((end - start) / PAGE_4KIB);
    size_t window_pages = PHYS_WINDOW_SIZE_32 / PAGE_4KIB;

    if (pages > window_pages)
        panic("phys window map too large");

    _window_lock_acquire();

    // single sliding window, new mappings invalidate any previous one
    if (window_pages_mapped) {
        if (window_stack_depth >= PHYS_WINDOW_STACK_MAX)
            panic("phys window map stack overflow");

        window_stack[window_stack_depth++] = (window_map_t){
            .paddr_base = window_paddr_base,
            .pages = window_pages_mapped,
            .flags = window_flags,
        };

        _clear_window_range(window_pages_mapped);
    }

    window_pages_mapped = pages;
    window_paddr_base = start;

    u64 pt_flags = _map_flags_to_pt_flags(flags);
    window_flags = pt_flags;

    u32 vaddr = PHYS_WINDOW_BASE_32;

    for (size_t i = 0; i < pages; i++)
        _map_window_page(
            vaddr + (u32)(i * PAGE_4KIB), start + i * PAGE_4KIB, pt_flags
        );

    return (void *)(uintptr_t)(PHYS_WINDOW_BASE_32 + (u32)(paddr - start));
}

void arch_phys_unmap(void *vaddr, size_t size) {
    (void)vaddr;
    (void)size;

    if (!window_pages_mapped)
        return;

    _clear_window_range(window_pages_mapped);
    window_pages_mapped = 0;
    window_paddr_base = 0;
    window_flags = PT_WRITE;

    if (!window_stack_depth) {
        _window_lock_release();
        return;
    }

    window_map_t prev = window_stack[--window_stack_depth];
    window_pages_mapped = prev.pages;
    window_paddr_base = prev.paddr_base;
    window_flags = prev.flags;

    u32 map_base = PHYS_WINDOW_BASE_32;

    for (size_t i = 0; i < prev.pages; i++) {
        _map_window_page(
            map_base + (u32)(i * PAGE_4KIB),
            prev.paddr_base + i * PAGE_4KIB,
            prev.flags
        );
    }

    _window_lock_release();
}

bool arch_phys_copy(u64 dst_paddr, u64 src_paddr, size_t size) {
    if (!size)
        return true;

    static const size_t kChunk = 256;
    u8 bounce[kChunk];

    size_t offset = 0;

    while (offset < size) {
        size_t chunk = size - offset;

        if (chunk > kChunk)
            chunk = kChunk;

        void *src = arch_phys_map(src_paddr + offset, chunk, 0);
        if (!src)
            return false;

        memcpy(bounce, src, chunk);
        arch_phys_unmap(src, chunk);

        void *dst = arch_phys_map(dst_paddr + offset, chunk, 0);
        if (!dst)
            return false;

        memcpy(dst, bounce, chunk);
        arch_phys_unmap(dst, chunk);

        offset += chunk;
    }

    return true;
}
#endif
