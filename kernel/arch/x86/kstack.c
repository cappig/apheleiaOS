#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <base/macros.h>
#include <sched/scheduler.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <x86/boot.h>

#if defined(__i386__)
static spinlock_t kstack_lock = SPINLOCK_INIT;
static uintptr_t kstack_floor = 0;
static uintptr_t kstack_next = 0;

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

static uintptr_t kstack_alloc_vaddr(size_t size) {
    if (!kstack_next) {
        kstack_floor = kstack_region_floor();
        kstack_next = kstack_region_top();
    }

    if (kstack_next < kstack_floor + size) {
        return 0;
    }

    kstack_next -= size;
    return kstack_next;
}
#endif

bool arch_kernel_stack_alloc(sched_thread_t *thread) {
    if (!thread || !thread->stack_size) {
        return false;
    }

#if defined(__i386__)
    size_t size = ALIGN(thread->stack_size, PAGE_4KIB);
    size_t pages = size / PAGE_4KIB;

    unsigned long flags = spin_lock_irqsave(&kstack_lock);
    uintptr_t vaddr = kstack_alloc_vaddr(size);
    spin_unlock_irqrestore(&kstack_lock, flags);

    if (!vaddr) {
        return false;
    }

    void *paddr = arch_alloc_frames_user(pages);
    if (!paddr) {
        return false;
    }

    void *root = arch_vm_root(arch_vm_kernel());
    if (!root) {
        arch_free_frames(paddr, pages);
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

    (void)thread->stack_size;

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
