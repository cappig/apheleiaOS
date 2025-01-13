#include "process.h"

#include <aos/syscalls.h>
#include <base/macros.h>
#include <base/types.h>
#include <data/vector.h>
#include <errno.h>
#include <parse/elf.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/paging.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "mem/virtual.h"
#include "sys/panic.h"


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


// Free all the buffers that we can and leave the process in a zombified state
bool process_free(process* proc) {
    if (!proc)
        return false;

    void* stack_paddr = (void*)ID_MAPPED_PADDR(proc->stack);
    free_frames(stack_paddr, proc->stack_size);

    if (proc->user.stack_size)
        free_frames((void*)proc->user.stack_paddr, proc->user.stack_size);

    free_table(proc->user.mem_map);
    free_frames(proc->user.mem_map, 1);

    proc->state = PROC_DONE;

    return true;
}

void process_destroy(process* proc) {
    if (!process_free(proc))
        return;

    kfree(proc);
    kfree(proc->name);
}


void process_init_stack(process* parent, process* child) {
    void* paddr = alloc_frames(SCHED_KSTACK_PAGES);

    child->stack = (u64)ID_MAPPED_VADDR(paddr);
    child->stack_size = SCHED_KSTACK_SIZE;

    usize offset = SCHED_KSTACK_SIZE;

    if (parent) {
        void* dest = (void*)child->stack;
        void* src = (void*)parent->stack;

        memcpy(dest, src, SCHED_KSTACK_SIZE);

        offset = parent->stack_ptr - parent->stack;
    }

    child->stack_ptr = child->stack + offset;
}

void process_init_page_map(process* parent, process* child) {
    assert(child->type == PROC_USER);

    page_table* table;

    if (parent)
        table = parent->user.mem_map;
    else
        table = (page_table*)read_cr3();

    child->user.mem_map = clone_table(table);
}

void process_init_file_descriptors(process* parent, process* child) {
    assert(child->type == PROC_USER);

    if (parent) {
        child->user.fd_table = vec_clone(parent->user.fd_table);
    } else {
        child->user.fd_table = vec_create(sizeof(file_desc));

        // Some reasonable defaults
        process_open_fd(child, "/dev/kbd", STDIN_FD);
        process_open_fd(child, "/dev/tty0", STDOUT_FD);
        process_open_fd(child, "/dev/tty0", STDERR_FD);
    }
}

isize process_open_fd(process* proc, const char* path, isize fd) {
    assert(proc->type == PROC_USER);

    file_desc fdesc = {
        .node = vfs_lookup(path),
        .offset = 0,
    };

    if (!fdesc.node)
        return -ENOENT;

    // A negative fd tells this function to assign it
    if (fd < 0)
        fd = proc->user.fd_table->size;

    if (!vec_insert(proc->user.fd_table, fd, &fdesc))
        return -ENOMEM;

    return fd;
}


void process_set_state(process* proc, int_state* state) {
    proc->stack_ptr -= sizeof(int_state);

    void* dest = (void*)proc->stack_ptr;
    memcpy(dest, state, sizeof(int_state));
}


// Spawns an orphaned kernel process that runs in ring 0
// Since these process have kernel level privileges they can run arbitrary kernel code so
// we only have to pass in a pointer to function that lives in the kernel's memory space
process* spawn_kproc(const char* name, void* entry) {
    usize pid = _next_pid();
    process* proc = process_create(name, PROC_KERNEL, pid);

    process_init_stack(NULL, proc);

    // All process must have a kernel stack that we use to save the process state on kernel entry
    // We also use it while in the kernel. A process may also have a 'user stack' that it uses
    // during execution Kernel processes can just use a single stack for both purposes
    u64 ksp = (u64)proc->stack_ptr - sizeof(int_state);

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
    process_init_file_descriptors(NULL, proc);

    return proc;
}


process* process_fork(process* parent) {
    assert(parent->type == PROC_USER);

    usize pid = _next_pid();
    process* child = process_create(parent->name, PROC_USER, pid);

    child->user.tree_entry = tree_create_node(child);
    tree_insert_child(parent->user.tree_entry, child->user.tree_entry);

    process_init_stack(parent, child);
    process_init_page_map(parent, child);
    process_init_file_descriptors(parent, child);

    return child;
}


bool process_exec_elf(process* proc, elf_header* header) {
    if (!header || !proc)
        return false;

    if (proc->type != PROC_USER)
        return false;

    if (elf_verify(header) != VALID_ELF)
        return false;

    // TODO: handle ET_DYN
    if (!elf_is_executable(header))
        return false;


    // Load the elf sections into memory
    for (usize i = 0; i < header->ph_num; i++) {
        elf_prog_header* p_header = (void*)header + header->phoff + i * header->phent_size;

        if (p_header->type != PT_LOAD)
            continue;

        if (!p_header->file_size && !p_header->mem_size)
            continue;


        usize pages = DIV_ROUND_UP(p_header->mem_size, PAGE_4KIB);

        u64 flags = elf_to_page_flags(p_header->flags);
        flags |= PT_USER;

        u64 pbase = (u64)alloc_frames(pages);
        u64 vbase = ALIGN_DOWN(p_header->vaddr, PAGE_4KIB);

        void* base = (void*)ID_MAPPED_VADDR(pbase);

        // Map the segment to virtual memory
        map_region(proc->user.mem_map, pages, vbase, pbase, flags);

        // Copy all loadable data from the file
        usize offset = p_header->vaddr - vbase;
        memcpy(base + offset, (void*)header + p_header->offset, p_header->file_size);

        // Zero out any additional space
        usize zero_len = p_header->mem_size - p_header->file_size;
        memset(base + p_header->file_size, 0, zero_len);
    }

    // Setup the 'user stack'
    // A nice round address near the top of the lower half of the canocial range
    // FIXME: this should not be hardcoded!
    proc->user.stack = 0x700000000000;

    proc->user.stack_size = SCHED_USTACK_SIZE;
    proc->user.stack_paddr = (u64)alloc_frames(SCHED_USTACK_PAGES);

    map_region(
        proc->user.mem_map,
        SCHED_USTACK_PAGES,
        proc->user.stack,
        proc->user.stack_paddr,
        PT_PRESENT | PT_WRITE | PT_USER | PT_NO_EXECUTE
    );

    // Set the starting state
    int_state state = {
        .s_regs.rip = (u64)header->entry,
        .s_regs.cs = GDT_user_code | 3,
        .s_regs.rflags = 0x200,
        .s_regs.rsp = proc->user.stack + proc->user.stack_size,
        .s_regs.ss = GDT_user_data | 3,
    };

    process_set_state(proc, &state);

    return true;
}
