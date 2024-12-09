#include "process.h"

#include <parse/elf.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/paging.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "mem/virtual.h"


static usize _next_pid(void) {
    static usize pid = 0;
    return pid++;
}


process* process_create(const char* name, u8 type, usize pid) {
    process* proc = kcalloc(sizeof(process));

    proc->name = strdup(name);
    proc->id = pid;

    proc->state = PROC_READY;
    proc->type = type;

    return proc;
}

void process_destroy(process* proc) {
    if (!proc)
        return;

    free_table(proc->cr3);
    free_frames(proc->cr3, 1);

    usize kstack_size = SCHED_KSTACK_PAGES * PAGE_4KIB;
    void* kstack_paddr = (void*)ID_MAPPED_PADDR(proc->kernel_stack);
    free_frames(kstack_paddr, kstack_size);

    kfree(proc);
}


void process_init_stack(process* parent, process* child) {
    void* paddr = alloc_frames(SCHED_KSTACK_PAGES);
    child->kernel_stack = (void*)ID_MAPPED_VADDR(paddr);

    usize offset = SCHED_STATE_OFFSET;
    if (parent) {
        memcpy(child->kernel_stack, parent->kernel_stack, SCHED_KSTAK_SIZE);
        offset = parent->ksp - parent->kernel_stack;
    }

    child->ksp = child->kernel_stack + offset;
}

void process_init_page_map(process* parent, process* child) {
    page_table* table;

    if (parent)
        table = parent->cr3;
    else
        table = (page_table*)read_cr3();

    child->cr3 = clone_table(table);
}


void process_set_state(process* proc, int_state* state) {
    void* dest = proc->kernel_stack + SCHED_STATE_OFFSET;
    memcpy(dest, state, sizeof(int_state));
}


// Spawns an orphaned kernel process that runs in ring 0
// Since these process have kernel level privileges they can run arbitrary kernel code so
// we only have to pass in a pointer to function that lives in the kernel's memory space
process* spawn_kproc(const char* name, void* entry) {
    usize pid = _next_pid();
    process* proc = process_create(name, PROC_KERNEL, pid);

    process_init_stack(NULL, proc);
    // process_init_page_map(NULL, proc);

    // All process must have a kernel stack that we use to save the process state on kernel entry
    // We also use it while in the kernel. A process may also have a 'user stack' that it uses
    // during execution Kernel processes can just use a single stack for both purposes
    u64 ksp = (u64)proc->kernel_stack + SCHED_KSTAK_SIZE;

    int_state state = {
        .s_regs.rip = (u64)entry,
        .s_regs.cs = GDT_kernel_code,
        .s_regs.rflags = 0x200,
        .s_regs.rsp = ksp,
        .s_regs.ss = GDT_kernel_data
    };

    process_set_state(proc, &state);

    return proc;
}

// Spawns an orphaned user process that runs in ring 3
// We use this to spawn 'init'
process* spawn_uproc(const char* name) {
    usize pid = _next_pid();
    process* proc = process_create(name, PROC_USER, pid);

    process_init_stack(NULL, proc);
    process_init_page_map(NULL, proc);

    return proc;
}


// Map an ELF executable into a user processes memory space
bool process_exec_elf(process* proc, elf_header* header) {
    if (!header || !proc)
        return false;

    if (proc->type != PROC_USER)
        return false;

    if (elf_verify(header) != VALID_ELF)
        return false;

    // TODO: handle ET_DYN
    if (header->type != ET_EXEC)
        return false;


    // Load the elf sections into memory
    for (usize i = 0; i < header->ph_num; i++) {
        elf_prog_header* p_header = (void*)header + header->phoff + i * header->phent_size;

        if (p_header->type != PT_LOAD)
            continue;

        if (!p_header->file_size && !p_header->mem_size)
            continue;

        u64 size = ALIGN(p_header->mem_size, PAGE_4KIB);

        u64 flags = elf_to_page_flags(p_header->flags);
        flags |= PT_USER | PT_PRESENT;

        u64 pbase = (u64)alloc_frames(size / PAGE_4KIB);
        u64 vbase = p_header->vaddr;

        void* base = (void*)ID_MAPPED_VADDR(pbase);

        // Map the segment to virtual memory
        map_region(proc->cr3, size, vbase, pbase, flags);

        // Copy all loadable data from the file
        memcpy(base, (void*)header + p_header->offset, p_header->file_size);

        // Zero out any additional space
        memset(base + p_header->file_size, 0, p_header->mem_size - p_header->file_size);
    }

    // Setup the 'user stack'
    // A nice round address near the top of the lower half of the canocial range
    u64 ustack = 0x700000000000;

    proc->user_stack_size = SCHED_USTACK_SIZE;
    proc->user_stack = alloc_frames(SCHED_USTACK_PAGES);

    u64 ustack_flags = PT_PRESENT | PT_WRITE | PT_USER | PT_NO_EXECUTE;

    map_region(proc->cr3, SCHED_USTACK_SIZE, ustack, (u64)proc->user_stack, ustack_flags);

    // Set the starting state
    int_state state = {
        .s_regs.rip = (u64)header->entry,
        .s_regs.cs = GDT_user_code | 3,
        .s_regs.rflags = 0x200,
        .s_regs.rsp = ustack + proc->user_stack_size,
        .s_regs.ss = GDT_user_data | 3,
    };

    process_set_state(proc, &state);

    return true;
}
