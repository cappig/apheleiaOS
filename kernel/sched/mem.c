#include "mem.h"

#include <arch/mm.h>
#include <arch/paging.h>
#include <base/macros.h>
#include <base/units.h>
#include <stdbool.h>
#include <stdlib.h>

static inline u64 _pages_to_kib(size_t pages) {
    return ((u64)pages * PAGE_4KIB) / KIB;
}

void sched_user_mem_add(sched_thread_t *thread, size_t pages) {
    if (!thread || !pages) {
        return;
    }

    u64 delta_kib = _pages_to_kib(pages);
    if (!delta_kib) {
        return;
    }

    __atomic_add_fetch(&thread->user_mem_kib, delta_kib, __ATOMIC_RELAXED);
}

void sched_user_mem_sub(sched_thread_t *thread, size_t pages) {
    if (!thread || !pages) {
        return;
    }

    u64 delta_kib = _pages_to_kib(pages);
    if (!delta_kib) {
        return;
    }

    u64 current = __atomic_load_n(&thread->user_mem_kib, __ATOMIC_RELAXED);
    while (current > 0) {
        u64 next = current > delta_kib ? (current - delta_kib) : 0;

        if (__atomic_compare_exchange_n(
                &thread->user_mem_kib,
                &current,
                next,
                false,
                __ATOMIC_RELAXED,
                __ATOMIC_RELAXED
            )) {

            return;
        }
    }
}

void sched_user_mem_set_kib(sched_thread_t *thread, u64 kib) {
    if (!thread) {
        return;
    }

    __atomic_store_n(&thread->user_mem_kib, kib, __ATOMIC_RELAXED);
}

u64 sched_user_mem_kib(const sched_thread_t *thread) {
    if (!thread) {
        return 0;
    }

    return __atomic_load_n(&thread->user_mem_kib, __ATOMIC_RELAXED);
}

bool sched_add_user_region(
    sched_thread_t *thread,
    uintptr_t vaddr,
    uintptr_t paddr,
    size_t pages,
    u64 flags
) {
    if (!thread || !pages) {
        return false;
    }

    sched_user_region_t *region = calloc(1, sizeof(*region));
    if (!region) {
        return false;
    }

    region->vaddr = vaddr;
    region->paddr = paddr;
    region->pages = pages;
    region->flags = flags;
    region->next = thread->regions;
    thread->regions = region;
    sched_user_mem_add(thread, pages);

    return true;
}

void sched_clear_user_regions(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    sched_user_region_t *region = thread->regions;
    while (region) {
        sched_user_region_t *next = region->next;

        if (region->paddr && region->pages) {
            arch_free_frames((void *)region->paddr, region->pages);
        }

        free(region);
        region = next;
    }

    thread->regions = NULL;
    sched_user_mem_set_kib(thread, 0);
}

static sched_user_region_t *
find_user_region(sched_thread_t *thread, uintptr_t addr) {
    if (!thread) {
        return NULL;
    }

    sched_user_region_t *region = thread->regions;

    while (region) {
        uintptr_t start = region->vaddr;
        uintptr_t end = start + region->pages * PAGE_4KIB;

        if (addr >= start && addr < end) {
            return region;
        }

        region = region->next;
    }

    return NULL;
}

bool sched_user_region_mark_cow(
    sched_thread_t *thread,
    sched_user_region_t *region
) {
    if (!thread || !region || !region->pages) {
        return false;
    }

    page_t *root = arch_vm_root(thread->vm_space);
    if (!root) {
        return false;
    }

    bool updated = false;
    for (size_t i = 0; i < region->pages; i++) {
        uintptr_t vaddr = region->vaddr + i * PAGE_4KIB;
        page_t *entry = NULL;

        size_t size = arch_get_page(root, vaddr, &entry);
        if (!entry || size != PAGE_4KIB) {
            continue;
        }

        if (*entry & PT_WRITE) {
            *entry &= ~PT_WRITE;
            updated = true;
        }
    }

    return updated;
}

static bool split_region_for_page(
    sched_user_region_t *region,
    size_t page_index,
    uintptr_t new_page_paddr,
    u64 new_flags
) {
    if (!region || !region->pages || page_index >= region->pages) {
        return false;
    }

    size_t before = page_index;
    size_t after = region->pages - page_index - 1;
    uintptr_t page_vaddr = region->vaddr + page_index * PAGE_4KIB;
    uintptr_t old_page_paddr = region->paddr + page_index * PAGE_4KIB;
    sched_user_region_t *next = region->next;

    if (!before && !after) {
        region->vaddr = page_vaddr;
        region->paddr = new_page_paddr;
        region->pages = 1;
        region->flags = new_flags;
        return true;
    }

    if (!before) {
        sched_user_region_t *after_region = NULL;
        if (after > 0) {
            after_region = calloc(1, sizeof(*after_region));

            if (!after_region) {
                return false;
            }

            after_region->vaddr = page_vaddr + PAGE_4KIB;
            after_region->paddr = old_page_paddr + PAGE_4KIB;
            after_region->pages = after;
            after_region->flags = region->flags;
            after_region->next = next;
        }

        region->vaddr = page_vaddr;
        region->paddr = new_page_paddr;
        region->pages = 1;
        region->flags = new_flags;
        region->next = after_region ? after_region : next;
        return true;
    }

    if (before > 0) {
        region->pages = before;

        sched_user_region_t *page_region = calloc(1, sizeof(*page_region));
        if (!page_region) {
            return false;
        }

        page_region->vaddr = page_vaddr;
        page_region->paddr = new_page_paddr;
        page_region->pages = 1;
        page_region->flags = new_flags;

        if (!after) {
            page_region->next = next;
            region->next = page_region;
            return true;
        }

        sched_user_region_t *after_region = calloc(1, sizeof(*after_region));
        if (!after_region) {
            free(page_region);
            return false;
        }

        after_region->vaddr = page_vaddr + PAGE_4KIB;
        after_region->paddr = old_page_paddr + PAGE_4KIB;
        after_region->pages = after;
        after_region->flags = region->flags;
        after_region->next = next;

        region->next = page_region;
        page_region->next = after_region;
        return true;
    }

    return false;
}

static bool upgrade_cow_region(page_t *root, sched_user_region_t *region) {
    if (!root || !region || !region->pages) {
        return false;
    }

    region->flags = (region->flags & ~SCHED_REGION_COW) | PT_WRITE;

    for (size_t i = 0; i < region->pages; i++) {
        uintptr_t vaddr = region->vaddr + i * PAGE_4KIB;
        page_t *entry = NULL;
        size_t size = arch_get_page(root, vaddr, &entry);

        if (!entry || size != PAGE_4KIB) {
            continue;
        }

        *entry |= PT_WRITE;
        arch_tlb_flush(vaddr);
    }

    return true;
}

bool sched_handle_cow_fault(
    sched_thread_t *thread,
    uintptr_t addr,
    bool write
) {
    if (!thread || !thread->user_thread || !write) {
        return false;
    }

    unsigned long vm_flags = spin_lock_irqsave(&thread->vm_lock);

    uintptr_t page_addr = ALIGN_DOWN(addr, PAGE_4KIB);
    sched_user_region_t *region = find_user_region(thread, page_addr);

    if (!region) {
        spin_unlock_irqrestore(&thread->vm_lock, vm_flags);
        return false;
    }

    if (!(region->flags & SCHED_REGION_COW)) {
        if (region->flags & PT_WRITE) {
            page_t *root = arch_vm_root(thread->vm_space);

            if (root) {
                page_t *entry = NULL;
                size_t size = arch_get_page(root, page_addr, &entry);

                if (entry && size == PAGE_4KIB && !(*entry & PT_WRITE)) {
                    *entry |= PT_WRITE;
                    arch_tlb_flush(page_addr);
                    spin_unlock_irqrestore(&thread->vm_lock, vm_flags);
                    return true;
                }
            }
        }

        spin_unlock_irqrestore(&thread->vm_lock, vm_flags);
        return false;
    }

    page_t *root = arch_vm_root(thread->vm_space);
    if (!root) {
        spin_unlock_irqrestore(&thread->vm_lock, vm_flags);
        return false;
    }

    page_t *entry = NULL;
    size_t size = arch_get_page(root, page_addr, &entry);

    if (!entry || size != PAGE_4KIB) {
        spin_unlock_irqrestore(&thread->vm_lock, vm_flags);
        return false;
    }

    if (*entry & PT_WRITE) {
        spin_unlock_irqrestore(&thread->vm_lock, vm_flags);
        return false;
    }

    u64 old_paddr = arch_page_get_paddr(entry);
    u16 refs = pmm_refcount((void *)(uintptr_t)old_paddr);
    u64 new_flags = (region->flags & ~SCHED_REGION_COW) | PT_WRITE;
    size_t page_index = (page_addr - region->vaddr) / PAGE_4KIB;

    if (!pmm_ref_ready()) {
        refs = 2;
    }

    if (refs <= 1) {
        bool upgraded = upgrade_cow_region(root, region);
        spin_unlock_irqrestore(&thread->vm_lock, vm_flags);
        return upgraded;
    }

    if (refs > 1) {
        uintptr_t new_paddr = (uintptr_t)arch_alloc_frames_user(1);

        if (!arch_phys_copy(new_paddr, old_paddr, PAGE_4KIB)) {
            arch_free_frames((void *)new_paddr, 1);
            spin_unlock_irqrestore(&thread->vm_lock, vm_flags);
            return false;
        }

        bool split_ok =
            split_region_for_page(region, page_index, new_paddr, new_flags);

        if (!split_ok) {
            arch_free_frames((void *)new_paddr, 1);
            spin_unlock_irqrestore(&thread->vm_lock, vm_flags);
            return false;
        }

        *entry = 0;
        arch_page_set_paddr(entry, new_paddr);
        *entry |= (new_flags | PT_PRESENT) & FLAGS_MASK;
        arch_free_frames((void *)(uintptr_t)old_paddr, 1);
    }

    arch_tlb_flush(page_addr);
    spin_unlock_irqrestore(&thread->vm_lock, vm_flags);

    return true;
}
