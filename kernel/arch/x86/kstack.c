#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <base/macros.h>
#include <sched/scheduler.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <x86/boot.h>

#if defined(__i386__)
#define KSTACK_PAGE_COUNT (KSTACK_REGION_SIZE_32 / PAGE_4KIB)
#define KSTACK_WORD_COUNT ((KSTACK_PAGE_COUNT + 63U) / 64U)

typedef struct {
    spinlock_t lock;
    uintptr_t floor;
    u64 used[KSTACK_WORD_COUNT];
} kstack_alloc_t;

static kstack_alloc_t kstack = {
    .lock = SPINLOCK_INIT,
};

static uintptr_t kstack_region_top(void) {
    return ALIGN_DOWN(KSTACK_REGION_TOP_32, PAGE_4KIB);
}

static uintptr_t kstack_region_floor(void) {
    extern char __kernel_end;
    uintptr_t kernel_end = ALIGN((uintptr_t)&__kernel_end, PAGE_4KIB);
    uintptr_t floor = KSTACK_REGION_BASE_32;
    if (floor < kernel_end) {
        floor = kernel_end;
    }
    return floor;
}

static bool kstack_page_used(size_t page) {
    return (kstack.used[page / 64U] & (1ULL << (page % 64U))) != 0;
}

static void kstack_mark_pages(size_t first, size_t pages, bool used) {
    for (size_t i = 0; i < pages; i++) {
        size_t page = first + i;
        u64 mask = 1ULL << (page % 64U);

        if (used) {
            kstack.used[page / 64U] |= mask;
        } else {
            kstack.used[page / 64U] &= ~mask;
        }
    }
}

// i386 kernel stacks use reusable page slots in the reserved stack window.
static uintptr_t kstack_alloc_vaddr(size_t size) {
    if (!kstack.floor) {
        kstack.floor = kstack_region_floor();
    }

    size_t pages = size / PAGE_4KIB;
    uintptr_t region_base = KSTACK_REGION_BASE_32;
    size_t first_page = DIV_ROUND_UP(kstack.floor - region_base, PAGE_4KIB);
    size_t end_page = (kstack_region_top() - region_base) / PAGE_4KIB;

    if (!pages || first_page > end_page || pages > end_page - first_page) {
        return 0;
    }

    for (size_t end = end_page; end >= first_page + pages; end--) {
        size_t first = end - pages;
        bool free = true;

        for (size_t page = first; page < end; page++) {
            if (kstack_page_used(page)) {
                free = false;
                break;
            }
        }

        if (free) {
            kstack_mark_pages(first, pages, true);
            return region_base + first * PAGE_4KIB;
        }

        if (end == first_page + pages) {
            break;
        }
    }

    return 0;
}

static void kstack_release_vaddr(uintptr_t vaddr, size_t size) {
    uintptr_t region_base = KSTACK_REGION_BASE_32;
    if (vaddr < region_base || (vaddr - region_base) % PAGE_4KIB || size % PAGE_4KIB) {
        return;
    }

    size_t first = (vaddr - region_base) / PAGE_4KIB;
    size_t pages = size / PAGE_4KIB;

    if (first >= KSTACK_PAGE_COUNT || pages > KSTACK_PAGE_COUNT - first) {
        return;
    }

    kstack_mark_pages(first, pages, false);
}
#endif

bool arch_kernel_stack_alloc(sched_thread_t *thread) {
    if (!thread || !thread->stack_size) {
        return false;
    }

#if defined(__i386__)
    if (thread->stack_size > SIZE_MAX - (PAGE_4KIB - 1)) {
        return false;
    }

    size_t size = ALIGN(thread->stack_size, PAGE_4KIB);
    size_t pages = size / PAGE_4KIB;

    unsigned long flags = spin_lock_irqsave(&kstack.lock);
    uintptr_t vaddr = kstack_alloc_vaddr(size);
    spin_unlock_irqrestore(&kstack.lock, flags);

    if (!vaddr) {
        return false;
    }

    void *paddr = arch_alloc_frames_user(pages);
    if (!paddr) {
        flags = spin_lock_irqsave(&kstack.lock);
        kstack_release_vaddr(vaddr, size);
        spin_unlock_irqrestore(&kstack.lock, flags);
        return false;
    }

    void *root = arch_vm_root(arch_vm_kernel());
    if (!root) {
        arch_free_frames(paddr, pages);
        flags = spin_lock_irqsave(&kstack.lock);
        kstack_release_vaddr(vaddr, size);
        spin_unlock_irqrestore(&kstack.lock, flags);
        return false;
    }

    arch_map_region(root, pages, vaddr, (uintptr_t)paddr, PT_WRITE);
    memset((void *)vaddr, 0, size);

    thread->stack = (void *)vaddr;
    thread->stack_size = size;
    return true;
#else
    thread->stack = malloc(thread->stack_size);
    return thread->stack != NULL;
#endif
}

void arch_kernel_stack_free(sched_thread_t *thread) {
    if (!thread || !thread->stack || !thread->stack_size) {
        return;
    }

#if defined(__i386__)
    uintptr_t base = (uintptr_t)thread->stack;
    size_t size = thread->stack_size;
    size_t pages = size / PAGE_4KIB;
    void *root = arch_vm_root(arch_vm_kernel());

    if (root) {
        for (size_t i = 0; i < pages; i++) {
            uintptr_t vaddr = base + i * PAGE_4KIB;
            page_t *entry = NULL;

            if (arch_get_page(root, vaddr, &entry) != PAGE_4KIB || !entry) {
                continue;
            }

            u64 paddr = arch_page_get_paddr(entry);
            unmap_page(root, vaddr);
            arch_tlb_flush(vaddr);
            arch_free_frames((void *)(uintptr_t)paddr, 1);
        }
    }

    unsigned long flags = spin_lock_irqsave(&kstack.lock);
    kstack_release_vaddr(base, size);
    spin_unlock_irqrestore(&kstack.lock, flags);
#else
    free(thread->stack);
#endif
    thread->stack = NULL;
}

bool arch_kernel_stack_valid(const sched_thread_t *thread) {
    if (!thread || !thread->stack || !thread->stack_size) {
        return false;
    }

#if defined(__i386__)
    uintptr_t base = (uintptr_t)thread->stack;
    uintptr_t end = base + thread->stack_size;

    if (end < base) {
        return false;
    }

    if (base < KSTACK_REGION_BASE_32 || end > KSTACK_REGION_TOP_32) {
        return false;
    }
#endif

    return true;
}
