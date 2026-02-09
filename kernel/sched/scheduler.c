#include "scheduler.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <log/log.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/panic.h>
#include <x86/asm.h>
#include <x86/gdt.h>

#define SCHED_STACK_SIZE (16 * KIB)
#define SCHED_SLICE      5

extern void arch_context_switch(uintptr_t stack_ptr) NORETURN;

static linked_list_t* run_queue = NULL;
static linked_list_t* zombie_list = NULL;
static sched_thread_t* current = NULL;
static sched_thread_t* idle_thread = NULL;

static bool sched_running = false;
static size_t ticks_left = SCHED_SLICE;

static void _sched_reap(void);

static NORETURN void _thread_trampoline(void) {
    sched_thread_t* thread = sched_current();
    if (thread && thread->entry)
        thread->entry(thread->arg);
    sched_exit();
    __builtin_unreachable();
}

static void _idle_entry(UNUSED void* arg) {
    for (;;) {
        _sched_reap();
        halt();
    }
}

static void _sched_reap(void) {
    if (!zombie_list)
        return;

    unsigned long flags = irq_save();
    list_node_t* node = zombie_list->head;

    while (node) {
        list_node_t* next = node->next;
        sched_thread_t* thread = node->data;

        if (thread && thread != current && thread != idle_thread && !thread->is_bootstrap) {
            list_remove(zombie_list, node);
            thread->in_zombie_list = false;

            if (thread->stack)
                free(thread->stack);

            free(thread);
        }

        node = next;
    }

    irq_restore(flags);
}

static uintptr_t _build_initial_stack(sched_thread_t* thread) {
    uintptr_t sp = (uintptr_t)thread->stack + thread->stack_size;
    sp = ALIGN_DOWN(sp, 16);

#if defined(__x86_64__)
    // Hardware frame for iretq plus padding to keep ABI alignment.
    uintptr_t stack_top = sp;
    uintptr_t entry_rsp = stack_top - 24;

    sp -= sizeof(u64);
    *(u64*)sp = 0; // padding
    // Provide a valid ring0 RSP/SS if iretq consumes them.
    sp -= sizeof(u64);
    *(u64*)sp = GDT_KERNEL_DATA; // ss
    sp -= sizeof(u64);
    *(u64*)sp = (u64)entry_rsp; // rsp
    sp -= sizeof(u64);
    *(u64*)sp = 0x202; // RFLAGS with IF set
    sp -= sizeof(u64);
    *(u64*)sp = GDT_KERNEL_CODE;
    sp -= sizeof(u64);
    *(u64*)sp = (u64)(uintptr_t)_thread_trampoline;

    // Error code and vector.
    sp -= sizeof(u64);
    *(u64*)sp = 0;
    sp -= sizeof(u64);
    *(u64*)sp = 0;

    // General registers in push order (matches isr stubs).
    u64 regs[15] = {0};

    for (size_t i = 0; i < ARRAY_LEN(regs); i++) {
        sp -= sizeof(u64);
        *(u64*)sp = regs[i];
    }
#else
    // Stack after iret: return address, entry, arg.
    sp -= sizeof(u32);
    *(u32*)sp = (u32)(uintptr_t)thread->arg;
    sp -= sizeof(u32);
    *(u32*)sp = (u32)(uintptr_t)thread->entry;
    sp -= sizeof(u32);
    *(u32*)sp = (u32)(uintptr_t)sched_exit;

    // Hardware frame for iret.
    sp -= sizeof(u32);
    *(u32*)sp = 0x202; // EFLAGS with IF set
    sp -= sizeof(u32);
    *(u32*)sp = GDT_KERNEL_CODE;
    sp -= sizeof(u32);
    *(u32*)sp = (u32)(uintptr_t)_thread_trampoline;

    // Error code and vector.
    sp -= sizeof(u32);
    *(u32*)sp = 0;
    sp -= sizeof(u32);
    *(u32*)sp = 0;

    // General registers in push order (matches isr stubs).
    u32 regs[7] = {
        0, // eax
        0, // ebx
        0, // ecx
        0, // edx
        0, // esi
        0, // edi
        0, // ebp
    };
    for (size_t i = 0; i < ARRAY_LEN(regs); i++) {
        sp -= sizeof(u32);
        *(u32*)sp = regs[i];
    }
#endif

    return sp;
}

static void _enqueue_thread(sched_thread_t* thread) {
    if (!thread || thread->in_run_queue || thread == idle_thread)
        return;

    thread->run_node.data = thread;
    list_append(run_queue, &thread->run_node);
    thread->in_run_queue = true;
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
_create_thread(const char* name, thread_entry_t entry, void* arg, bool enqueue) {
    sched_thread_t* thread = calloc(1, sizeof(*thread));
    if (!thread)
        return NULL;

    thread->name = name ? name : "thread";
    thread->entry = entry;
    thread->arg = arg;
    thread->state = THREAD_READY;
    thread->stack_size = SCHED_STACK_SIZE;
    thread->stack = malloc(thread->stack_size);

    if (!thread->stack) {
        free(thread);
        return NULL;
    }

    thread->context = _build_initial_stack(thread);

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

    current = calloc(1, sizeof(*current));
    assert(current);

    current->name = "bootstrap";
    current->state = THREAD_RUNNING;
    current->is_bootstrap = true;

    idle_thread = _create_thread("idle", _idle_entry, NULL, false);
    assert(idle_thread);

    ticks_left = SCHED_SLICE;
    sched_running = false;
}

void scheduler_start(void) {
    sched_running = true;
}

bool sched_is_running(void) {
    return sched_running;
}

sched_thread_t* sched_create_kernel_thread(const char* name, thread_entry_t entry, void* arg) {
    return _create_thread(name, entry, arg, true);
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

void sched_block_locked(sched_wait_queue_t* queue, unsigned long flags) {
    if (!sched_running || !queue || !queue->list || !current) {
        irq_restore(flags);
        return;
    }

    current->state = THREAD_SLEEPING;
    _wait_queue_append(queue, current);

    irq_restore(flags);

    while (current->state == THREAD_SLEEPING) {
        sched_yield();
        asm volatile("hlt");
    }
}

void sched_block(sched_wait_queue_t* queue) {
    unsigned long flags = irq_save();
    sched_block_locked(queue, flags);
}

void sched_wake_one(sched_wait_queue_t* queue) {
    if (!queue || !queue->list)
        return;

    unsigned long flags = irq_save();
    sched_thread_t* thread = _wait_queue_pop(queue);

    if (thread) {
        thread->state = THREAD_READY;
        _enqueue_thread(thread);
    }

    irq_restore(flags);
}

void sched_wake_all(sched_wait_queue_t* queue) {
    if (!queue || !queue->list)
        return;

    unsigned long flags = irq_save();

    for (;;) {
        sched_thread_t* thread = _wait_queue_pop(queue);
        if (!thread)
            break;

        thread->state = THREAD_READY;
        _enqueue_thread(thread);
    }

    irq_restore(flags);
}

void sched_tick(arch_int_state_t* state) {
    if (!sched_running || !state || !current)
        return;

    _sched_reap();

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

    next->state = THREAD_RUNNING;
    current = next;

    arch_context_switch(next->context);
}

void sched_yield(void) {
    if (!sched_running || !current)
        return;

    _sched_reap();
    ticks_left = 0;
}

void sched_exit(void) {
    disable_interrupts();

    if (current) {
        current->state = THREAD_ZOMBIE;
        if (current != idle_thread && !current->is_bootstrap && !current->in_zombie_list) {
            current->zombie_node.data = current;
            list_append(zombie_list, &current->zombie_node);
            current->in_zombie_list = true;
        }
    }

    sched_thread_t* next = _pick_next_thread();
    if (!next) {
        for (;;)
            halt();
    }

    current = next;
    next->state = THREAD_RUNNING;
    arch_context_switch(next->context);
}
