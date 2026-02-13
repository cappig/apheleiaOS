#include "scheduler.h"

#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <base/units.h>
#include <errno.h>
#include <log/log.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/panic.h>
#include <sys/proc.h>
#include <sys/tty.h>
#include <sys/wait.h>

#define SCHED_STACK_SIZE (16 * KIB)
#define SCHED_SLICE      5

static linked_list_t* run_queue = NULL;
static linked_list_t* zombie_list = NULL;
static linked_list_t* all_list = NULL;
static arch_vm_space_t* kernel_vm = NULL;

static pid_t next_pid = 1;

static bool sched_running = false;
typedef struct {
    sched_thread_t* current;
    sched_thread_t* idle_thread;
    size_t ticks_left;
    size_t preempt_depth;
} sched_cpu_state_t;

static sched_cpu_state_t sched_cpu_state[MAX_CORES] = {0};

static volatile int sched_lock = 0;
static size_t sched_lock_owner = (size_t)-1;
static size_t sched_lock_depth = 0;

static size_t _sched_cpu_id(void) {
    cpu_core_t* core = cpu_current();

    if (!core || core->id >= MAX_CORES)
        return 0;

    return core->id;
}

static sched_cpu_state_t* _sched_local(void) {
    return &sched_cpu_state[_sched_cpu_id()];
}

static unsigned long _sched_lock_save(void) {
    unsigned long flags = arch_irq_save();
    size_t cpu_id = _sched_cpu_id();

    if (sched_lock_owner == cpu_id) {
        sched_lock_depth++;
        return flags;
    }

    while (__sync_lock_test_and_set(&sched_lock, 1)) {
        while (sched_lock)
            arch_cpu_wait();
    }

    sched_lock_owner = cpu_id;
    sched_lock_depth = 1;

    return flags;
}

static void _sched_lock_restore(unsigned long flags) {
    size_t cpu_id = _sched_cpu_id();

    if (sched_lock_owner == cpu_id) {
        if (sched_lock_depth > 1) {
            sched_lock_depth--;
        } else {
            sched_lock_depth = 0;
            sched_lock_owner = (size_t)-1;
            __sync_lock_release(&sched_lock);
        }
    }

    arch_irq_restore(flags);
}

static pid_t _sched_next_pid(void) {
    unsigned long flags = _sched_lock_save();
    pid_t pid = next_pid++;
    _sched_lock_restore(flags);
    return pid;
}

static inline sched_thread_t* _sched_local_current(void) {
    return _sched_local()->current;
}

static inline void _sched_local_set_current(sched_thread_t* thread) {
    _sched_local()->current = thread;
}

static inline sched_thread_t* _sched_local_idle(void) {
    return _sched_local()->idle_thread;
}

static inline void _sched_local_set_idle(sched_thread_t* thread) {
    _sched_local()->idle_thread = thread;
}

static inline size_t _sched_local_ticks_left(void) {
    return _sched_local()->ticks_left;
}

static inline void _sched_local_set_ticks_left(size_t ticks) {
    _sched_local()->ticks_left = ticks;
}

static inline void _sched_local_dec_ticks_left(void) {
    if (_sched_local()->ticks_left > 0)
        _sched_local()->ticks_left--;
}

static inline void _sched_local_inc_preempt_depth(void) {
    _sched_local()->preempt_depth++;
}

static inline void _sched_local_dec_preempt_depth(void) {
    if (_sched_local()->preempt_depth > 0)
        _sched_local()->preempt_depth--;
}

static inline bool _sched_local_preempt_disabled(void) {
    return _sched_local()->preempt_depth != 0;
}

static proc_state_t _sched_state_to_proc(thread_state_t state) {
    switch (state) {
    case THREAD_READY:
        return PROC_STATE_READY;
    case THREAD_RUNNING:
        return PROC_STATE_RUNNING;
    case THREAD_SLEEPING:
        return PROC_STATE_SLEEPING;
    case THREAD_STOPPED:
        return PROC_STATE_STOPPED;
    case THREAD_ZOMBIE:
        return PROC_STATE_ZOMBIE;
    default:
        return PROC_STATE_READY;
    }
}

static void _add_all_thread(sched_thread_t* thread) {
    if (!thread || !all_list)
        return;

    unsigned long flags = _sched_lock_save();

    if (thread->in_all_list) {
        _sched_lock_restore(flags);
        return;
    }

    thread->all_node.data = thread;
    list_append(all_list, &thread->all_node);
    thread->in_all_list = true;
    _sched_lock_restore(flags);
}

static void _sched_fd_reset(sched_fd_t* fd) {
    if (!fd)
        return;

    fd->kind = SCHED_FD_NONE;
    fd->node = NULL;
    fd->pipe = NULL;
    fd->offset = 0;
    fd->flags = 0;
}

sched_pipe_t* sched_pipe_create(size_t capacity) {
    if (!capacity)
        capacity = SCHED_PIPE_CAPACITY;

    sched_pipe_t* pipe = calloc(1, sizeof(*pipe));
    if (!pipe)
        return NULL;

    pipe->data = calloc(capacity, sizeof(u8));
    pipe->read_wait_queue = calloc(1, sizeof(sched_wait_queue_t));
    pipe->write_wait_queue = calloc(1, sizeof(sched_wait_queue_t));

    if (!pipe->data || !pipe->read_wait_queue || !pipe->write_wait_queue) {
        free(pipe->data);
        free(pipe->read_wait_queue);
        free(pipe->write_wait_queue);
        free(pipe);

        return NULL;
    }

    pipe->capacity = capacity;
    pipe->read_wait_owned = true;
    pipe->write_wait_owned = true;
    sched_wait_queue_init(pipe->read_wait_queue);
    sched_wait_queue_init(pipe->write_wait_queue);

    return pipe;
}

static void _sched_pipe_try_destroy(sched_pipe_t* pipe) {
    if (!pipe)
        return;

    if (pipe->readers || pipe->writers)
        return;

    if (pipe->read_wait_owned && pipe->read_wait_queue)
        sched_wait_queue_destroy(pipe->read_wait_queue);
    if (pipe->write_wait_owned && pipe->write_wait_queue)
        sched_wait_queue_destroy(pipe->write_wait_queue);

    free(pipe->read_wait_queue);
    free(pipe->write_wait_queue);
    free(pipe->data);
    free(pipe);
}

void sched_pipe_acquire_reader(sched_pipe_t* pipe) {
    if (!pipe)
        return;

    unsigned long flags = _sched_lock_save();
    pipe->readers++;
    _sched_lock_restore(flags);
}

void sched_pipe_acquire_writer(sched_pipe_t* pipe) {
    if (!pipe)
        return;

    unsigned long flags = _sched_lock_save();
    pipe->writers++;
    _sched_lock_restore(flags);
}

void sched_pipe_release_reader(sched_pipe_t* pipe) {
    if (!pipe)
        return;

    bool destroy = false;
    unsigned long flags = _sched_lock_save();

    if (pipe->readers > 0)
        pipe->readers--;

    destroy = !pipe->readers && !pipe->writers;
    _sched_lock_restore(flags);

    if (pipe->write_wait_queue)
        sched_wake_all(pipe->write_wait_queue);
    if (pipe->read_wait_queue)
        sched_wake_all(pipe->read_wait_queue);

    if (destroy)
        _sched_pipe_try_destroy(pipe);
}

void sched_pipe_release_writer(sched_pipe_t* pipe) {
    if (!pipe)
        return;

    bool destroy = false;
    unsigned long flags = _sched_lock_save();

    if (pipe->writers > 0)
        pipe->writers--;

    destroy = !pipe->readers && !pipe->writers;
    _sched_lock_restore(flags);

    if (pipe->read_wait_queue)
        sched_wake_all(pipe->read_wait_queue);
    if (pipe->write_wait_queue)
        sched_wake_all(pipe->write_wait_queue);

    if (destroy)
        _sched_pipe_try_destroy(pipe);
}

static void _sched_fd_retain(const sched_fd_t* fd) {
    if (!fd)
        return;

    if (fd->kind == SCHED_FD_PIPE_READ)
        sched_pipe_acquire_reader(fd->pipe);
    else if (fd->kind == SCHED_FD_PIPE_WRITE)
        sched_pipe_acquire_writer(fd->pipe);
}

static void _sched_fd_release_value(sched_fd_t* fd) {
    if (!fd)
        return;

    if (fd->kind == SCHED_FD_PIPE_READ)
        sched_pipe_release_reader(fd->pipe);
    else if (fd->kind == SCHED_FD_PIPE_WRITE)
        sched_pipe_release_writer(fd->pipe);

    _sched_fd_reset(fd);
}

int sched_fd_alloc(sched_thread_t* thread, const sched_fd_t* fd, int min_fd) {
    if (!thread || !fd || fd->kind == SCHED_FD_NONE)
        return -EINVAL;

    int start = min_fd < 0 ? 0 : min_fd;

    if (start >= SCHED_FD_MAX)
        return -EMFILE;

    for (int slot = start; slot < SCHED_FD_MAX; slot++) {
        if (thread->fd_used[slot])
            continue;

        thread->fd_used[slot] = true;
        thread->fds[slot] = *fd;
        _sched_fd_retain(&thread->fds[slot]);
        return slot;
    }

    return -EMFILE;
}

int sched_fd_close(sched_thread_t* thread, int fd) {
    if (!thread || fd < 0 || fd >= SCHED_FD_MAX || !thread->fd_used[fd])
        return -EBADF;

    sched_fd_t old = thread->fds[fd];
    thread->fd_used[fd] = false;

    sched_fd_reset(&thread->fds[fd]);
    sched_fd_release_value(&old);

    return 0;
}

int sched_fd_install(sched_thread_t* thread, int target_fd, const sched_fd_t* fd) {
    if (!thread || !fd || fd->kind == SCHED_FD_NONE)
        return -EINVAL;

    if (target_fd < 0 || target_fd >= SCHED_FD_MAX)
        return -EBADF;

    if (thread->fd_used[target_fd])
        sched_fd_close(thread, target_fd);

    thread->fd_used[target_fd] = true;
    thread->fds[target_fd] = *fd;

    sched_fd_retain(&thread->fds[target_fd]);

    return target_fd;
}

int sched_fd_dup(sched_thread_t* thread, int oldfd, int newfd) {
    if (!thread || oldfd < 0 || oldfd >= SCHED_FD_MAX || !thread->fd_used[oldfd])
        return -EBADF;

    if (newfd < 0 || newfd >= SCHED_FD_MAX)
        return -EBADF;

    if (oldfd == newfd)
        return newfd;

    sched_fd_t source = thread->fds[oldfd];
    return sched_fd_install(thread, newfd, &source);
}

bool sched_fd_clone_table(sched_thread_t* dst, const sched_thread_t* src) {
    if (!dst || !src)
        return false;

    for (int fd = 0; fd < SCHED_FD_MAX; fd++) {
        if (!src->fd_used[fd])
            continue;

        dst->fd_used[fd] = true;
        dst->fds[fd] = src->fds[fd];
        _sched_fd_retain(&dst->fds[fd]);
    }

    return true;
}

void sched_fd_close_all(sched_thread_t* thread) {
    if (!thread)
        return;

    for (int fd = 0; fd < SCHED_FD_MAX; fd++) {
        if (!thread->fd_used[fd])
            continue;

        sched_fd_close(thread, fd);
    }
}

static void _sched_init_thread_name(sched_thread_t* thread, const char* name) {
    if (!thread) {
        return;
    }

    const char* src = name ? name : "thread";
    memset(thread->name, 0, sizeof(thread->name));

    size_t len = strnlen(src, sizeof(thread->name) - 1);

    memcpy(thread->name, src, len);
    thread->name[len] = '\0';
}

static void _remove_all_thread(sched_thread_t* thread) {
    if (!thread || !all_list)
        return;

    unsigned long flags = _sched_lock_save();

    if (!thread->in_all_list) {
        _sched_lock_restore(flags);
        return;
    }

    list_remove(all_list, &thread->all_node);
    thread->in_all_list = false;
    _sched_lock_restore(flags);
}

static sched_thread_t* _find_thread_by_pid(pid_t pid) {
    if (!all_list)
        return NULL;

    unsigned long flags = _sched_lock_save();

    ll_foreach(node, all_list) {
        sched_thread_t* thread = node->data;

        if (thread && thread->pid == pid) {
            _sched_lock_restore(flags);
            return thread;
        }
    }

    _sched_lock_restore(flags);
    return NULL;
}

static NORETURN void _thread_trampoline(void) {
    sched_thread_t* thread = sched_current();

    if (thread && thread->entry)
        thread->entry(thread->arg);

    sched_exit();
    __builtin_unreachable();
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

    if (!before && !after) {
        region->vaddr = page_vaddr;
        region->paddr = new_page_paddr;
        region->pages = 1;
        region->flags = new_flags;
        return true;
    }

    if (!before) {
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

        if (!after) {
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
    sched_user_region_t* region = find_user_region(thread, page_addr);

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

        if (!arch_phys_copy(new_paddr, old_paddr, PAGE_4KIB))
            return false;

        *entry = 0;
        arch_page_set_paddr(entry, new_paddr);
        *entry |= (new_flags | PT_PRESENT) & FLAGS_MASK;

        if (!split_region_for_page(region, (page_addr - region->vaddr) / PAGE_4KIB, new_paddr, new_flags))
            return false;

        arch_free_frames((void*)(uintptr_t)old_paddr, 1);
    } else {
        *entry = 0;
        arch_page_set_paddr(entry, old_paddr);
        *entry |= (new_flags | PT_PRESENT) & FLAGS_MASK;

        if (!split_region_for_page(region, (page_addr - region->vaddr) / PAGE_4KIB, (uintptr_t)old_paddr, new_flags))
            return false;
    }

    arch_tlb_flush(page_addr);
    return true;
}

static void _destroy_thread(sched_thread_t* thread) {
    if (!thread)
        return;

    sched_fd_close_all(thread);

    if (thread->vm_space && thread->vm_space != kernel_vm)
        arch_vm_destroy(thread->vm_space);

    sched_clear_user_regions(thread);

    sched_wait_queue_destroy(&thread->wait_queue);

    if (thread->stack)
        free(thread->stack);

    _remove_all_thread(thread);

    free(thread);
}

static void _sched_reap(void) {
    if (!zombie_list)
        return;

    unsigned long flags = _sched_lock_save();
    list_node_t* node = zombie_list->head;

    while (node) {
        list_node_t* next = node->next;
        sched_thread_t* thread = node->data;

        if (thread && thread != _sched_local_current() && thread != _sched_local_idle()) {
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

    _sched_lock_restore(flags);
}

static void _idle_entry(UNUSED void* arg) {
    for (;;) {
        _sched_reap();
        arch_cpu_wait();
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
    if (!thread || thread == _sched_local_idle())
        return;

    unsigned long flags = _sched_lock_save();

    if (thread->in_run_queue)
        goto out;

    thread->run_node.data = thread;
    list_append(run_queue, &thread->run_node);
    thread->in_run_queue = true;

out:
    _sched_lock_restore(flags);
}

static void _run_queue_remove(sched_thread_t* thread) {
    if (!thread || !thread->in_run_queue || !run_queue)
        return;

    unsigned long flags = _sched_lock_save();

    if (thread->in_run_queue && run_queue) {
        list_remove(run_queue, &thread->run_node);
        thread->in_run_queue = false;
    }

    _sched_lock_restore(flags);
}

static void _sched_wake_sleepers(u64 now) {
    if (!all_list)
        return;

    unsigned long flags = _sched_lock_save();

    ll_foreach(node, all_list) {
        sched_thread_t* thread = node->data;
        if (!thread || thread->state != THREAD_SLEEPING)
            continue;

        if (!thread->wake_tick)
            continue;

        if (thread->wake_tick > now)
            continue;

        thread->wake_tick = 0;
        thread->state = THREAD_READY;
        _enqueue_thread(thread);
    }

    _sched_lock_restore(flags);
}

static void _wait_queue_remove(sched_thread_t* thread) {
    if (!thread || !thread->in_wait_queue || !thread->blocked_on || !thread->blocked_on->list)
        return;

    unsigned long flags = _sched_lock_save();

    if (thread->in_wait_queue && thread->blocked_on && thread->blocked_on->list) {
        list_remove(thread->blocked_on->list, &thread->wait_node);
        thread->in_wait_queue = false;
        thread->blocked_on = NULL;
    }

    _sched_lock_restore(flags);
}

static void _wait_queue_append(sched_wait_queue_t* queue, sched_thread_t* thread) {
    if (!queue || !queue->list || !thread || thread->in_wait_queue)
        return;

    unsigned long flags = _sched_lock_save();

    if (thread->in_wait_queue || !queue || !queue->list || !thread)
        goto out;

    thread->wait_node.data = thread;
    list_append(queue->list, &thread->wait_node);
    thread->in_wait_queue = true;
    thread->blocked_on = queue;

out:
    _sched_lock_restore(flags);
}

void sched_discard_thread(sched_thread_t* thread) {
    if (!thread)
        return;

    _run_queue_remove(thread);
    wait_queue_remove(thread);

    if (thread->in_zombie_list && zombie_list) {
        unsigned long flags = _sched_lock_save();

        if (thread->in_zombie_list) {
            list_remove(zombie_list, &thread->zombie_node);
            thread->in_zombie_list = false;
        }

        _sched_lock_restore(flags);
    }

    destroy_thread(thread);
}

void sched_make_runnable(sched_thread_t* thread) {
    if (!thread)
        return;

    unsigned long flags = _sched_lock_save();

    wait_queue_remove(thread);
    thread->wake_tick = 0;
    thread->state = THREAD_READY;
    _enqueue_thread(thread);

    _sched_lock_restore(flags);
}

void sched_unblock_thread(sched_thread_t* thread) {
    if (!thread)
        return;

    unsigned long flags = _sched_lock_save();

    wait_queue_remove(thread);

    if (thread->state == THREAD_SLEEPING || thread->state == THREAD_READY) {
        thread->wake_tick = 0;
        thread->state = THREAD_READY;
        _enqueue_thread(thread);
    }

    _sched_lock_restore(flags);
}

void sched_stop_thread(sched_thread_t* thread, int signum) {
    if (!thread || thread->state == THREAD_ZOMBIE || thread->state == THREAD_STOPPED)
        return;

    unsigned long flags = _sched_lock_save();

    _run_queue_remove(thread);
    _wait_queue_remove(thread);

    thread->wake_tick = 0;
    thread->state = THREAD_STOPPED;
    thread->stop_signal = signum;
    thread->stop_reported = false;

    if (thread->user_thread) {
        sched_thread_t* parent = _find_thread_by_pid(thread->ppid);

        if (parent) {
            sched_wake_one(&parent->wait_queue);
            sched_signal_send_thread(parent, SIGCHLD);
        }
    }

    _sched_lock_restore(flags);

    if (thread == _sched_local_current() && sched_running)
        sched_yield();
}

void sched_continue_thread(sched_thread_t* thread) {
    if (!thread || thread->state != THREAD_STOPPED)
        return;

    unsigned long flags = _sched_lock_save();

    thread->state = THREAD_READY;
    thread->stop_signal = 0;
    thread->stop_reported = false;
    _enqueue_thread(thread);

    _sched_lock_restore(flags);
}

static sched_thread_t* _dequeue_thread(void) {
    unsigned long flags = _sched_lock_save();

    list_node_t* node = list_pop_front(run_queue);
    if (!node) {
        _sched_lock_restore(flags);
        return NULL;
    }

    sched_thread_t* thread = node->data;
    if (!thread) {
        _sched_lock_restore(flags);
        return NULL;
    }

    thread->in_run_queue = false;
    _sched_lock_restore(flags);

    return thread;
}

static sched_thread_t* _pick_next_thread(void) {
    sched_thread_t* next = _dequeue_thread();
    if (next)
        return next;

    if (_sched_local_idle())
        return _sched_local_idle();

    return _sched_local_current();
}

static sched_thread_t*
_create_thread(const char* name, thread_entry_t entry, void* arg, bool enqueue, bool user_thread) {
    sched_thread_t* thread = calloc(1, sizeof(*thread));
    if (!thread)
        return NULL;

    sched_init_thread_name(thread, name);

    thread->entry = entry;
    thread->arg = arg;
    thread->state = THREAD_READY;
    thread->user_thread = user_thread;
    thread->pid = _sched_next_pid();
    thread->ppid = 0;

    sched_thread_t* parent = _sched_local_current();

    if (parent && parent->pid != 0) {
        thread->pgid = parent->pgid;
        thread->sid = parent->sid;
    } else {
        thread->pgid = thread->pid;
        thread->sid = thread->pid;
    }

    thread->uid = parent ? parent->uid : 0;
    thread->gid = parent ? parent->gid : 0;
    thread->umask = parent ? parent->umask : 0022;
    thread->stack_size = SCHED_STACK_SIZE;
    thread->stack = malloc(thread->stack_size);
    thread->tty_index = parent ? parent->tty_index : -1;

    if (!thread->stack) {
        free(thread);
        return NULL;
    }

    thread->cwd[0] = '/';
    thread->cwd[1] = '\0';

    if (!user_thread) {
        thread->context = build_initial_stack(thread);
    }

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
    return _sched_local_current();
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

    next_pid = 0;
    sched_thread_t* idle = create_thread("idle", idle_entry, NULL, false, false);
    assert(idle);
    idle->state = THREAD_RUNNING;
    idle->tty_index = TTY_NONE;

    _sched_local_set_idle(idle);
    _sched_local_set_current(idle);

    _sched_local_set_ticks_left(SCHED_SLICE);
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
    sched_thread_t* parent = _sched_local_current();

    if (!parent || !parent->user_thread || !state)
        return -1;

    sched_thread_t* child = sched_create_user_thread(parent->name);
    if (!child)
        return -1;

    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->umask = parent->umask;
    child->user_stack_base = parent->user_stack_base;
    child->user_stack_size = parent->user_stack_size;

    if (!sched_fd_clone_table(child, parent)) {
        sched_discard_thread(child);
        return -1;
    }

    memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    memcpy(child->signal_handlers, parent->signal_handlers, sizeof(child->signal_handlers));

    child->signal_mask = parent->signal_mask;
    child->signal_trampoline = parent->signal_trampoline;
    child->signal_pending = 0;
    child->signal_saved_valid = false;
    child->current_signal = 0;
    child->tty_index = parent->tty_index;

    bool cow_enabled = pmm_ref_ready();

    sched_user_region_t* region = parent->regions;
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
            mark_cow_range(parent, region);

        region = region->next;
    }

    child->context = _build_fork_stack(child, state);

    _enqueue_thread(child);
    return child->pid;
}

pid_t sched_wait(pid_t pid, int* status) {
    return sched_waitpid(pid, status, 0);
}

static bool
_waitpid_target_matches(const sched_thread_t* parent, const sched_thread_t* child, pid_t pid) {
    if (!parent || !child || !child->user_thread)
        return false;

    if (child->ppid != parent->pid)
        return false;

    if (pid > 0)
        return child->pid == pid;

    if (!pid)
        return child->pgid == parent->pgid;

    if (pid == -1)
        return true;

    return child->pgid == -pid;
}

pid_t sched_waitpid(pid_t pid, int* status, int options) {
    sched_thread_t* self = _sched_local_current();
    if (!self || !self->user_thread)
        return -ECHILD;

    for (;;) {
        sched_thread_t* found = NULL;
        sched_thread_t* stopped = NULL;
        bool has_matching_child = false;

        unsigned long flags = _sched_lock_save();

        if (!zombie_list || !all_list) {
            _sched_lock_restore(flags);
            return -ECHILD;
        }

        ll_foreach(node, zombie_list) {
            sched_thread_t* thread = node->data;
            if (!waitpid_target_matches(self, thread, pid))
                continue;

            found = thread;
            break;
        }

        if (found) {
            if (status)
                *status = found->exit_code;

            list_remove(zombie_list, &found->zombie_node);
            found->in_zombie_list = false;

            _sched_lock_restore(flags);

            pid_t ret = found->pid;
            _destroy_thread(found);
            return ret;
        }

        if (options & WUNTRACED) {
            ll_foreach(node, all_list) {
                sched_thread_t* thread = node->data;

                if (!waitpid_target_matches(self, thread, pid))
                    continue;

                has_matching_child = true;

                if (thread->state != THREAD_STOPPED || thread->stop_reported)
                    continue;

                stopped = thread;
                break;
            }

            if (stopped) {
                stopped->stop_reported = true;
                if (status)
                    *status = 0x7f | ((stopped->stop_signal & 0xff) << 8);
                _sched_lock_restore(flags);
                return stopped->pid;
            }
        } else {
            ll_foreach(node, all_list) {
                sched_thread_t* thread = node->data;
                if (waitpid_target_matches(self, thread, pid)) {
                    has_matching_child = true;
                    break;
                }
            }
        }

        if (!has_matching_child) {
            _sched_lock_restore(flags);
            return -ECHILD;
        }

        if (options & WNOHANG) {
            _sched_lock_restore(flags);
            return 0;
        }

        if (!sched_running) {
            _sched_lock_restore(flags);
            return -ECHILD;
        }

        _run_queue_remove(self);

        self->wake_tick = 0;
        self->state = THREAD_SLEEPING;
        wait_queue_append(&self->wait_queue, self);

        _sched_lock_restore(flags);

        while (self->state == THREAD_SLEEPING) {
            sched_yield();
            arch_cpu_wait();
        }
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

void sched_set_thread_name(sched_thread_t* thread, const char* name) {
    _sched_init_thread_name(thread, name);
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
    sched_thread_t* self = _sched_local_current();

    if (!sched_running || !queue || !queue->list || !self) {
        return;
    }

    unsigned long flags = _sched_lock_save();

    _run_queue_remove(self);

    self->wake_tick = 0;
    self->state = THREAD_SLEEPING;
    wait_queue_append(queue, self);

    _sched_lock_restore(flags);

    while (self->state == THREAD_SLEEPING) {
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

    unsigned long flags = _sched_lock_save();
    sched_thread_t* thread = wait_queue_pop(queue);

    if (thread) {
        thread->state = THREAD_READY;
        _enqueue_thread(thread);
    }

    _sched_lock_restore(flags);
}

void sched_wake_all(sched_wait_queue_t* queue) {
    if (!queue || !queue->list)
        return;

    unsigned long flags = _sched_lock_save();

    for (;;) {
        sched_thread_t* thread = _wait_queue_pop(queue);
        if (!thread)
            break;

        thread->state = THREAD_READY;
        _enqueue_thread(thread);
    }

    _sched_lock_restore(flags);
}

void sched_tick(arch_int_state_t* state) {
    static bool logged_first_switch = false;
    sched_thread_t* thread = _sched_local_current();

    if (!sched_running || !state || !thread)
        return;

    _sched_reap();
    _sched_wake_sleepers(arch_timer_ticks());

    thread->context = (uintptr_t)state;
    sched_signal_deliver_current(state);

    if (sched_preempt_disabled())
        return;

    _sched_local_dec_ticks_left();

    if (_sched_local_ticks_left() > 0)
        return;

    _sched_local_set_ticks_left(SCHED_SLICE);

    if (thread->state == THREAD_RUNNING && thread != _sched_local_idle()) {
        thread->state = THREAD_READY;
        _enqueue_thread(thread);
    }

    sched_thread_t* next = pick_next_thread();
    if (!next || next == thread) {
        thread->state = THREAD_RUNNING;
        return;
    }

    if (!logged_first_switch) {
        log_info(
            "scheduler: switching to %s (pid=%ld)",
            next->name[0] ? next->name : "thread",
            (long)next->pid
        );
        logged_first_switch = true;
    }

    next->state = THREAD_RUNNING;
    _sched_local_set_current(next);

    arch_set_kernel_stack((uintptr_t)next->stack + next->stack_size);
    arch_vm_switch(next->vm_space);
    arch_context_switch(next->context);
}

void sched_yield(void) {
    if (!sched_running || !_sched_local_current())
        return;

    _sched_reap();
    _sched_local_set_ticks_left(0);
}

void sched_sleep(u64 ticks) {
    sched_thread_t* self = _sched_local_current();
    if (!self || !ticks)
        return;

    if (!sched_running) {
        u64 start = arch_timer_ticks();
        while ((arch_timer_ticks() - start) < ticks)
            arch_cpu_wait();
        return;
    }

    u64 deadline = arch_timer_ticks() + ticks;
    self->wake_tick = deadline;

    for (;;) {
        self->state = THREAD_SLEEPING;
        sched_yield();

        while (self->state == THREAD_SLEEPING)
            arch_cpu_wait();

        if (arch_timer_ticks() >= deadline)
            break;

        if (self->state == THREAD_ZOMBIE)
            break;

        if (sched_signal_has_pending(self))
            break;
    }

    self->wake_tick = 0;
}

void sched_exit(void) {
    arch_irq_disable();

    unsigned long flags = _sched_lock_save();

    sched_thread_t* self = _sched_local_current();

    if (self) {
        self->state = THREAD_ZOMBIE;

        if (self != _sched_local_idle() && !self->in_zombie_list) {
            self->zombie_node.data = self;
            list_append(zombie_list, &self->zombie_node);
            self->in_zombie_list = true;
        }

        if (self->user_thread) {
            ll_foreach(node, all_list) {
                sched_thread_t* child = node->data;
                if (!child || child->ppid != self->pid)
                    continue;

                child->ppid = 1;
                if (child->state == THREAD_ZOMBIE) {
                    sched_thread_t* init = _find_thread_by_pid(1);
                    if (init)
                        sched_wake_one(&init->wait_queue);
                }
            }

            sched_thread_t* parent = _find_thread_by_pid(self->ppid);
            if (parent)
                sched_wake_one(&parent->wait_queue);
        }
    }

    sched_thread_t* next = pick_next_thread();

    _sched_lock_restore(flags);

    if (!next) {
        for (;;)
            arch_cpu_halt();
    }

    _sched_local_set_current(next);
    next->state = THREAD_RUNNING;
    arch_set_kernel_stack((uintptr_t)next->stack + next->stack_size);
    arch_vm_switch(next->vm_space);
    arch_context_switch(next->context);
}

void sched_preempt_disable(void) {
    _sched_local_inc_preempt_depth();
}

void sched_preempt_enable(void) {
    _sched_local_dec_preempt_depth();
}

bool sched_preempt_disabled(void) {
    return _sched_local_preempt_disabled();
}

size_t sched_list_procs(proc_info_t* out, size_t capacity) {
    if (!out || !capacity || !all_list)
        return 0;

    unsigned long flags = _sched_lock_save();
    size_t count = 0;

    ll_foreach(node, all_list) {
        if (count >= capacity)
            break;

        sched_thread_t* thread = node->data;
        if (!thread)
            continue;

        proc_info_t* info = &out[count++];

        info->pid = thread->pid;
        info->ppid = thread->ppid;
        info->pgid = thread->pgid;
        info->sid = thread->sid;
        info->uid = thread->uid;
        info->gid = thread->gid;
        info->state = _sched_state_to_proc(thread->state);
        info->tty_index = thread->tty_index;

        memset(info->name, 0, sizeof(info->name));
        strncpy(info->name, thread->name, sizeof(info->name) - 1);
    }

    _sched_lock_restore(flags);
    return count;
}

int sched_signal_send_pgrp(pid_t pgid, int signum) {
    if (!all_list || pgid <= 0)
        return -1;

    int count = 0;
    unsigned long flags = _sched_lock_save();

    ll_foreach(node, all_list) {
        sched_thread_t* thread = node->data;

        if (!thread || thread->pgid != pgid)
            continue;

        if (sched_signal_send_thread(thread, signum) >= 0)
            count++;
    }

    _sched_lock_restore(flags);
    return count ? count : -1;
}
