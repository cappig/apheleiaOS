#include "scheduler.h"

#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <log/log.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/panic.h>

#define SCHED_STACK_SIZE (16 * KIB)
#define SCHED_SLICE      5

extern void arch_context_switch(uintptr_t stack_ptr) NORETURN;

static linked_list_t* run_queue = NULL;
static linked_list_t* zombie_list = NULL;
static linked_list_t* all_list = NULL;
static sched_thread_t* current = NULL;
static sched_thread_t* idle_thread = NULL;
static arch_vm_space_t* kernel_vm = NULL;

static pid_t next_pid = 1;

static bool sched_running = false;
static size_t ticks_left = SCHED_SLICE;

static void _sched_reap(void);

static void _add_all_thread(sched_thread_t* thread) {
    if (!thread || !all_list || thread->in_all_list)
        return;

    thread->all_node.data = thread;
    list_append(all_list, &thread->all_node);
    thread->in_all_list = true;
}

static void _remove_all_thread(sched_thread_t* thread) {
    if (!thread || !all_list || !thread->in_all_list)
        return;

    list_remove(all_list, &thread->all_node);
    thread->in_all_list = false;
}

static sched_thread_t* _find_thread_by_pid(pid_t pid) {
    if (!all_list)
        return NULL;

    ll_foreach(node, all_list) {
        sched_thread_t* thread = node->data;
        if (thread && thread->pid == pid)
            return thread;
    }

    return NULL;
}

static NORETURN void _thread_trampoline(void) {
    sched_thread_t* thread = sched_current();
    if (thread && thread->entry)
        thread->entry(thread->arg);
    sched_exit();
    __builtin_unreachable();
}

static void _idle_entry(UNUSED void* arg) {
    for (;;) {
        sched_reap();
        arch_cpu_wait();
    }
}

bool sched_add_user_region(
    sched_thread_t* thread,
    uintptr_t vaddr,
    uintptr_t paddr,
    size_t pages,
    u64 flags
) {
    if (!thread || !pages)
        return false;

    sched_user_region_t* region = calloc(1, sizeof(*region));
    if (!region)
        return false;

    region->vaddr = vaddr;
    region->paddr = paddr;
    region->pages = pages;
    region->flags = flags;
    region->next = thread->regions;
    thread->regions = region;

    return true;
}

void sched_clear_user_regions(sched_thread_t* thread) {
    if (!thread)
        return;

    sched_user_region_t* region = thread->regions;

    while (region) {
        sched_user_region_t* next = region->next;

        if (region->paddr && region->pages)
            arch_free_frames((void*)region->paddr, region->pages);

        free(region);
        region = next;
    }

    thread->regions = NULL;
}

static sched_user_region_t* _find_user_region(sched_thread_t* thread, uintptr_t addr) {
    if (!thread)
        return NULL;

    sched_user_region_t* region = thread->regions;
    while (region) {
        uintptr_t start = region->vaddr;
        uintptr_t end = start + region->pages * PAGE_4KIB;
        if (addr >= start && addr < end)
            return region;
        region = region->next;
    }

    return NULL;
}

static void _mark_cow_range(sched_thread_t* thread, sched_user_region_t* region) {
    if (!thread || !region || !region->pages)
        return;

    page_t* root = arch_vm_root(thread->vm_space);
    if (!root)
        return;

    for (size_t i = 0; i < region->pages; i++) {
        uintptr_t vaddr = region->vaddr + i * PAGE_4KIB;
        page_t* entry = NULL;
        size_t size = arch_get_page(root, vaddr, &entry);
        if (!entry || size != PAGE_4KIB)
            continue;

        if (*entry & PT_WRITE) {
            *entry &= ~PT_WRITE;
            arch_tlb_flush(vaddr);
        }
    }
}

static bool _split_region_for_page(
    sched_user_region_t* region,
    size_t page_index,
    uintptr_t new_page_paddr,
    u64 new_flags
) {
    if (!region || !region->pages || page_index >= region->pages)
        return false;

    size_t before = page_index;
    size_t after = region->pages - page_index - 1;
    uintptr_t page_vaddr = region->vaddr + page_index * PAGE_4KIB;
    uintptr_t old_page_paddr = region->paddr + page_index * PAGE_4KIB;
    sched_user_region_t* next = region->next;

    if (before == 0 && after == 0) {
        region->vaddr = page_vaddr;
        region->paddr = new_page_paddr;
        region->pages = 1;
        region->flags = new_flags;
        return true;
    }

    if (before == 0) {
        sched_user_region_t* after_region = NULL;
        if (after > 0) {
            after_region = calloc(1, sizeof(*after_region));
            if (!after_region)
                return false;

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

        sched_user_region_t* page_region = calloc(1, sizeof(*page_region));
        if (!page_region)
            return false;

        page_region->vaddr = page_vaddr;
        page_region->paddr = new_page_paddr;
        page_region->pages = 1;
        page_region->flags = new_flags;

        if (after == 0) {
            page_region->next = next;
            region->next = page_region;
            return true;
        }

        sched_user_region_t* after_region = calloc(1, sizeof(*after_region));
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

bool sched_handle_cow_fault(sched_thread_t* thread, uintptr_t addr, bool write) {
    if (!thread || !thread->user_thread || !write)
        return false;

    uintptr_t page_addr = ALIGN_DOWN(addr, PAGE_4KIB);
    sched_user_region_t* region = _find_user_region(thread, page_addr);
    if (!region)
        return false;

    if (!(region->flags & SCHED_REGION_COW))
        return false;

    page_t* root = arch_vm_root(thread->vm_space);
    if (!root)
        return false;

    page_t* entry = NULL;
    size_t size = arch_get_page(root, page_addr, &entry);
    if (!entry || size != PAGE_4KIB)
        return false;

    if (*entry & PT_WRITE)
        return false;

    u64 old_paddr = arch_page_get_paddr(entry);
    u16 refs = pmm_refcount((void*)(uintptr_t)old_paddr);
    u64 new_flags = (region->flags & ~SCHED_REGION_COW) | PT_WRITE;

    if (!pmm_ref_ready())
        refs = 2;

    if (refs > 1) {
        uintptr_t new_paddr = (uintptr_t)arch_alloc_frames_user(1);
        void* src = arch_phys_map(old_paddr, PAGE_4KIB);
        void* dst = arch_phys_map(new_paddr, PAGE_4KIB);

        if (!src || !dst) {
            if (src)
                arch_phys_unmap(src, PAGE_4KIB);
            if (dst)
                arch_phys_unmap(dst, PAGE_4KIB);
            return false;
        }

        memcpy(dst, src, PAGE_4KIB);
        arch_phys_unmap(src, PAGE_4KIB);
        arch_phys_unmap(dst, PAGE_4KIB);

        *entry = 0;
        arch_page_set_paddr(entry, new_paddr);
        *entry |= (new_flags | PT_PRESENT) & FLAGS_MASK;

        if (!_split_region_for_page(
                region, (page_addr - region->vaddr) / PAGE_4KIB, new_paddr, new_flags
            ))
            return false;

        arch_free_frames((void*)(uintptr_t)old_paddr, 1);
    } else {
        *entry = 0;
        arch_page_set_paddr(entry, old_paddr);
        *entry |= (new_flags | PT_PRESENT) & FLAGS_MASK;

        if (!_split_region_for_page(
                region, (page_addr - region->vaddr) / PAGE_4KIB, (uintptr_t)old_paddr, new_flags
            ))
            return false;
    }

    arch_tlb_flush(page_addr);
    return true;
}

static void _destroy_thread(sched_thread_t* thread) {
    if (!thread)
        return;

    if (thread->vm_space && thread->vm_space != kernel_vm)
        arch_vm_destroy(thread->vm_space);

    sched_clear_user_regions(thread);

    sched_wait_queue_destroy(&thread->wait_queue);

    if (thread->stack)
        free(thread->stack);

    _remove_all_thread(thread);

    free(thread);
}

void sched_discard_thread(sched_thread_t* thread) {
    if (!thread)
        return;

    if (thread->in_run_queue && run_queue)
        list_remove(run_queue, &thread->run_node);

    _destroy_thread(thread);
}

static void _sched_reap(void) {
    if (!zombie_list)
        return;

    unsigned long flags = arch_irq_save();
    list_node_t* node = zombie_list->head;

    while (node) {
        list_node_t* next = node->next;
        sched_thread_t* thread = node->data;

        if (thread && thread != current && thread != idle_thread && !thread->is_bootstrap) {
            if (thread->user_thread) {
                node = next;
                continue;
            }
            list_remove(zombie_list, node);
            thread->in_zombie_list = false;

            _destroy_thread(thread);
        }

        node = next;
    }

    arch_irq_restore(flags);
}

static void _sched_wake_sleepers(u64 now) {
    if (!all_list)
        return;

    ll_foreach(node, all_list) {
        sched_thread_t* thread = node->data;
        if (!thread || thread->state != THREAD_SLEEPING)
            continue;

        if (thread->wake_tick == 0)
            continue;

        if (thread->wake_tick > now)
            continue;

        thread->wake_tick = 0;
        thread->state = THREAD_READY;
        _enqueue_thread(thread);
    }
}

static uintptr_t _build_initial_stack(sched_thread_t* thread) {
    return arch_build_kernel_stack(thread, (uintptr_t)thread_trampoline);
}

static uintptr_t
_build_user_stack(sched_thread_t* thread, uintptr_t entry, uintptr_t user_stack_top) {
    uintptr_t sp = (uintptr_t)thread->stack + thread->stack_size;
    sp = ALIGN_DOWN(sp, 16);

    sp -= sizeof(arch_int_state_t);
    arch_int_state_t* frame = (arch_int_state_t*)sp;
    memset(frame, 0, sizeof(*frame));

    arch_word_t user_sp = (arch_word_t)ALIGN_DOWN(user_stack_top, 16);
    arch_state_set_user_entry(frame, (arch_word_t)entry, user_sp);

    return sp;
}

static uintptr_t _build_fork_stack(sched_thread_t* thread, arch_int_state_t* state) {
    if (!thread || !state)
        return 0;

    uintptr_t sp = (uintptr_t)thread->stack + thread->stack_size;
    sp = ALIGN_DOWN(sp, 16);

    sp -= sizeof(*state);
    memcpy((void*)sp, state, sizeof(*state));

    arch_int_state_t* child_state = (arch_int_state_t*)sp;

    arch_state_set_return(child_state, 0);

    return sp;
}

void sched_prepare_user_thread(sched_thread_t* thread, uintptr_t entry, uintptr_t user_stack_top) {
    if (!thread)
        return;

    thread->context = _build_user_stack(thread, entry, user_stack_top);
}

static void _enqueue_thread(sched_thread_t* thread) {
    if (!thread || thread == idle_thread)
        return;

    if (thread->in_run_queue)
        return;

    thread->run_node.data = thread;
    list_append(run_queue, &thread->run_node);
    thread->in_run_queue = true;
}

void sched_make_runnable(sched_thread_t* thread) {
    if (!thread)
        return;

    thread->state = THREAD_READY;
    _enqueue_thread(thread);
}

static sched_thread_t* _dequeue_thread(void) {
    list_node_t* node = list_pop_front(run_queue);
    if (!node)
        return NULL;

    sched_thread_t* thread = node->data;
    if (!thread)
        return NULL;

    thread->in_run_queue = false;
    return thread;
}

static sched_thread_t* _pick_next_thread(void) {
    sched_thread_t* next = _dequeue_thread();
    if (next)
        return next;

    return idle_thread ? idle_thread : current;
}

static sched_thread_t*
_create_thread(const char* name, thread_entry_t entry, void* arg, bool enqueue, bool user_thread) {
    sched_thread_t* thread = calloc(1, sizeof(*thread));
    if (!thread)
        return NULL;

    thread->name = name ? name : "thread";
    thread->entry = entry;
    thread->arg = arg;
    thread->state = THREAD_READY;
    thread->user_thread = user_thread;
    thread->pid = next_pid++;
    thread->ppid = 0;
    thread->stack_size = SCHED_STACK_SIZE;
    thread->stack = malloc(thread->stack_size);

    if (!thread->stack) {
        free(thread);
        return NULL;
    }

    if (!user_thread)
        thread->context = _build_initial_stack(thread);

    if (user_thread) {
        thread->vm_space = arch_vm_create_user();
        if (!thread->vm_space) {
            free(thread->stack);
            free(thread);
            return NULL;
        }
    } else {
        thread->vm_space = kernel_vm;
    }

    sched_wait_queue_init(&thread->wait_queue);
    _add_all_thread(thread);

    if (enqueue)
        _enqueue_thread(thread);

    return thread;
}

sched_thread_t* sched_current(void) {
    return current;
}

void scheduler_init(void) {
    if (run_queue)
        return;

    run_queue = list_create();
    assert(run_queue);

    zombie_list = list_create();
    assert(zombie_list);

    all_list = list_create();
    assert(all_list);

    kernel_vm = arch_vm_kernel();
    assert(kernel_vm);

    current = calloc(1, sizeof(*current));
    assert(current);

    current->name = "bootstrap";
    current->state = THREAD_RUNNING;
    current->is_bootstrap = true;
    current->pid = 0;
    current->ppid = 0;
    current->vm_space = kernel_vm;
    sched_wait_queue_init(&current->wait_queue);
    _add_all_thread(current);

    idle_thread = _create_thread("idle", idle_entry, NULL, false, false);
    assert(idle_thread);

    ticks_left = SCHED_SLICE;
    sched_running = false;
}

void scheduler_start(void) {
    log_info("scheduler: starting");
    sched_running = true;
}

bool sched_is_running(void) {
    return sched_running;
}

sched_thread_t* sched_create_kernel_thread(const char* name, thread_entry_t entry, void* arg) {
    return _create_thread(name, entry, arg, true, false);
}

sched_thread_t* sched_create_user_thread(const char* name) {
    return _create_thread(name, NULL, NULL, false, true);
}

pid_t sched_fork(arch_int_state_t* state) {
    if (!current || !current->user_thread || !state)
        return -1;

    sched_thread_t* child = sched_create_user_thread(current->name);
    if (!child)
        return -1;

    child->ppid = current->pid;
    child->user_stack_base = current->user_stack_base;
    child->user_stack_size = current->user_stack_size;

    bool cow_enabled = pmm_ref_ready();

    sched_user_region_t* region = current->regions;
    while (region) {
        size_t pages = region->pages;
        void* root = arch_vm_root(child->vm_space);
        if (!root) {
            sched_discard_thread(child);
            return -1;
        }

        if (!cow_enabled) {
            size_t size = pages * PAGE_4KIB;
            uintptr_t new_paddr = (uintptr_t)arch_alloc_frames_user(pages);

            arch_map_region(root, pages, region->vaddr, new_paddr, region->flags);
            sched_add_user_region(child, region->vaddr, new_paddr, pages, region->flags);

            void* dst = arch_phys_map(new_paddr, size);
            if (!dst) {
                sched_discard_thread(child);
                return -1;
            }

            memcpy(dst, (void*)region->vaddr, size);
            arch_phys_unmap(dst, size);

            region = region->next;
            continue;
        }

        u64 region_flags = region->flags;
        bool writable = (region_flags & PT_WRITE) != 0;

        if (writable)
            region_flags |= SCHED_REGION_COW;

        region->flags = region_flags;

        u64 map_flags = region_flags;
        if (writable)
            map_flags &= ~PT_WRITE;

        arch_map_region(root, pages, region->vaddr, region->paddr, map_flags);
        sched_add_user_region(child, region->vaddr, region->paddr, pages, region_flags);

        pmm_ref_hold((void*)(uintptr_t)region->paddr, pages);

        if (writable)
            _mark_cow_range(current, region);

        region = region->next;
    }

    child->context = _build_fork_stack(child, state);

    _enqueue_thread(child);
    return child->pid;
}

pid_t sched_wait(pid_t pid, int* status) {
    if (!current || !current->user_thread)
        return -1;

    for (;;) {
        sched_thread_t* found = NULL;

        if (!zombie_list)
            return -1;

        ll_foreach(node, zombie_list) {
            sched_thread_t* thread = node->data;
            if (!thread || !thread->user_thread)
                continue;

            if (thread->ppid != current->pid)
                continue;

            if (pid > 0 && thread->pid != pid)
                continue;

            found = thread;
            break;
        }

        if (found) {
            if (status)
                *status = found->exit_code;

            list_remove(zombie_list, &found->zombie_node);
            found->in_zombie_list = false;

            pid_t ret = found->pid;
            _destroy_thread(found);
            return ret;
        }

        if (!sched_running)
            return -1;

        sched_block(&current->wait_queue);
    }
}

void sched_wait_queue_init(sched_wait_queue_t* queue) {
    if (!queue)
        return;

    if (!queue->list)
        queue->list = list_create();

    assert(queue->list);
}

void sched_wait_queue_destroy(sched_wait_queue_t* queue) {
    if (!queue || !queue->list)
        return;

    list_destroy(queue->list, false);
    queue->list = NULL;
}

static void _wait_queue_append(sched_wait_queue_t* queue, sched_thread_t* thread) {
    if (!queue || !queue->list || !thread || thread->in_wait_queue)
        return;

    thread->wait_node.data = thread;
    list_append(queue->list, &thread->wait_node);
    thread->in_wait_queue = true;
}

static sched_thread_t* _wait_queue_pop(sched_wait_queue_t* queue) {
    if (!queue || !queue->list)
        return NULL;

    list_node_t* node = list_pop_front(queue->list);
    if (!node)
        return NULL;

    sched_thread_t* thread = node->data;
    if (!thread)
        return NULL;

    thread->in_wait_queue = false;
    return thread;
}

void sched_block(sched_wait_queue_t* queue) {
    if (!sched_running || !queue || !queue->list || !current) {
        return;
    }

    unsigned long flags = arch_irq_save();

    if (current->in_run_queue && run_queue) {
        list_remove(run_queue, &current->run_node);
        current->in_run_queue = false;
    }

    current->wake_tick = 0;
    current->state = THREAD_SLEEPING;
    _wait_queue_append(queue, current);

    arch_irq_restore(flags);

    while (current->state == THREAD_SLEEPING) {
        sched_yield();
        arch_cpu_wait();
    }

}

void sched_block(sched_wait_queue_t* queue) {
    unsigned long flags = irq_save();
    sched_block_locked(queue, flags);
}

void sched_wake_one(sched_wait_queue_t* queue) {
    if (!queue || !queue->list)
        return;

    unsigned long flags = arch_irq_save();
    sched_thread_t* thread = wait_queue_pop(queue);

    if (thread) {
        thread->state = THREAD_READY;
        _enqueue_thread(thread);
    }

    arch_irq_restore(flags);
}

void sched_wake_all(sched_wait_queue_t* queue) {
    if (!queue || !queue->list)
        return;

    unsigned long flags = arch_irq_save();

    for (;;) {
        sched_thread_t* thread = _wait_queue_pop(queue);
        if (!thread)
            break;

        thread->state = THREAD_READY;
        _enqueue_thread(thread);
    }

    arch_irq_restore(flags);
}

void sched_tick(arch_int_state_t* state) {
    static bool logged_first_switch = false;
    if (!sched_running || !state || !current)
        return;

    sched_reap();
    _sched_wake_sleepers(arch_timer_ticks());

    current->context = (uintptr_t)state;

    if (ticks_left > 0)
        ticks_left--;

    if (ticks_left > 0)
        return;

    ticks_left = SCHED_SLICE;

    if (current && current->state == THREAD_RUNNING && current != idle_thread &&
        !current->is_bootstrap) {
        current->state = THREAD_READY;
        _enqueue_thread(current);
    }

    sched_thread_t* next = _pick_next_thread();
    if (!next || next == current) {
        current->state = THREAD_RUNNING;
        return;
    }

    if (!logged_first_switch) {
        log_info(
            "scheduler: switching to %s (pid=%ld)",
            next->name ? next->name : "thread",
            (long)next->pid
        );
        logged_first_switch = true;
    }

    next->state = THREAD_RUNNING;
    current = next;

    arch_set_kernel_stack((uintptr_t)next->stack + next->stack_size);
    arch_vm_switch(next->vm_space);
    arch_context_switch(next->context);
}

void sched_yield(void) {
    if (!sched_running || !current)
        return;

    _sched_reap();
    ticks_left = 0;
}

void sched_sleep(u64 ticks) {
    if (!current || ticks == 0)
        return;

    if (!sched_running) {
        u64 start = arch_timer_ticks();
        while ((arch_timer_ticks() - start) < ticks)
            arch_cpu_wait();
        return;
    }

    current->wake_tick = arch_timer_ticks() + ticks;
    current->state = THREAD_SLEEPING;
    sched_yield();

    while (current->state == THREAD_SLEEPING)
        arch_cpu_wait();
}

void sched_exit(void) {
    arch_irq_disable();

    if (current) {
        current->state = THREAD_ZOMBIE;
        if (current != idle_thread && !current->is_bootstrap && !current->in_zombie_list) {
            current->zombie_node.data = current;
            list_append(zombie_list, &current->zombie_node);
            current->in_zombie_list = true;
        }

        if (current->user_thread) {
            sched_thread_t* parent = _find_thread_by_pid(current->ppid);
            if (parent)
                sched_wake_one(&parent->wait_queue);
        }
    }

    sched_thread_t* next = _pick_next_thread();
    if (!next) {
        for (;;)
            arch_cpu_halt();
    }

    current = next;
    next->state = THREAD_RUNNING;
    arch_set_kernel_stack((uintptr_t)next->stack + next->stack_size);
    arch_vm_switch(next->vm_space);
    arch_context_switch(next->context);
}
