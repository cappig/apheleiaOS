#include "process.h"

#include <aos/syscalls.h>
#include <base/addr.h>
#include <base/macros.h>
#include <base/types.h>
#include <data/vector.h>
#include <errno.h>
#include <fs/ustar.h>
#include <log/log.h>
#include <parse/elf.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/paging.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "data/list.h"
#include "data/tree.h"
#include "drivers/initrd.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "mem/virtual.h"
#include "sched/scheduler.h"
#include "sched/signal.h"
#include "sys/panic.h"
#include "vfs/fs.h"


static void* vdso_elf;

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

    // Init adopts any orphaned children
    tree_node* proc_node = proc->user.tree_entry;
    tree_node* init_node = proc_tree->root;

    foreach (node, proc_node->children) {
        tree_node* child_node = node->data;
        tree_insert_child(init_node, child_node);

#ifdef SCHED_DEBUG
        log_debug("[SCHED_DEBUG] init adopted orphan: name=%s pid=%lu", proc->name, proc->id);
#endif
    }

    list_destroy(proc_node->children);
    proc_node->children = NULL;

    return true;
}

// obvously process_free() has to be called before so that nothing is left dangling
pid_t process_reap(process* proc) {
    pid_t ret = proc->id;

#ifdef SCHED_DEBUG
    log_debug("[SCHED_DEBUG] reaping process: name=%s pid=%lu", proc->name, proc->id);
#endif

    tree_node* tnode = proc->user.tree_entry;
    tree_remove_child(tnode->parent, tnode);

    assert(!tnode->children);
    tree_destroy_node(tnode);

    kfree(proc);
    kfree(proc->name);

    return ret;
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

        // Clone the user stack
        if (child->type == PROC_USER) {
            child->user.stack_vaddr = parent->user.stack_vaddr;
            child->user.stack_size = parent->user.stack_size;

            usize pages = DIV_ROUND_UP(parent->user.stack_size, PAGE_4KIB);

            child->user.stack_paddr = (u64)alloc_frames(pages);

            void* child_vaddr = (void*)ID_MAPPED_VADDR(child->user.stack_paddr);
            void* parent_vaddr = (void*)ID_MAPPED_VADDR(parent->user.stack_paddr);

            memcpy(child_vaddr, parent_vaddr, parent->user.stack_size);
        }
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

void process_init_signal_handlers(process* parent, process* child) {
    assert(child->type == PROC_USER);

    if (parent) {
        usize len = SIGNAL_COUNT * sizeof(sighandler_fn);
        memcpy(child->user.signals.handlers, parent->user.signals.handlers, len);

        child->user.signals.masked = parent->user.signals.masked;
    } else {
        process_signal_defaults(child);
    }
}


isize process_open_fd(process* proc, const char* path, isize fd) {
    assert(proc->type == PROC_USER);

    // TODO: flags and mode
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
    proc->stack_ptr = proc->stack + proc->stack_size - sizeof(int_state);

    void* dest = (void*)proc->stack_ptr;
    memcpy(dest, state, sizeof(int_state));
}

void process_push_state(process* proc, int_state* state) {
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
    // during execution in userspace. Kernel processes can just use a single stack for both purposes
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
    process_init_signal_handlers(NULL, proc);

    return proc;
}


static bool _init_ustack(process* proc, u64 base, usize size) {
    if (proc->user.stack_vaddr)
        return false;

    usize pages = DIV_ROUND_UP(size, PAGE_4KIB);

    proc->user.stack_vaddr = base;

    proc->user.stack_size = size;
    proc->user.stack_paddr = (u64)alloc_frames(pages);

    map_region(
        proc->user.mem_map,
        SCHED_USTACK_PAGES,
        proc->user.stack_vaddr,
        proc->user.stack_paddr,
        PT_PRESENT | PT_NO_EXECUTE | PT_WRITE | PT_USER
    );

    // TODO: map a dummy canary page at the bottom of the stack to detect overflow
    // Writes to this page will fault, and we can go from there
    map_page(proc->user.mem_map, PAGE_4KIB, base, 0, PT_NO_EXECUTE);

    return true;
}

// Returns the highest virtual address in the image map
static u64 _load_elf_sections(process* proc, elf_header* header, usize load_offset) {
    u64 top = 0;

    for (usize i = 0; i < header->ph_num; i++) {
        elf_prog_header* p_header = (void*)header + header->phoff + i * header->phent_size;

        if (p_header->type != PT_LOAD)
            continue;

        if (!p_header->file_size && !p_header->mem_size)
            continue;

        usize pages = DIV_ROUND_UP(p_header->mem_size, PAGE_4KIB);

        u64 flags = elf_to_page_flags(p_header->flags);
        flags |= PT_USER;

        u64 vaddr = p_header->vaddr + load_offset;

        u64 pbase = (u64)alloc_frames(pages);
        u64 vbase = ALIGN_DOWN(vaddr, PAGE_4KIB);

        void* base = (void*)ID_MAPPED_VADDR(pbase);

        u64 seg_top = vbase + pages * PAGE_4KIB;

        if (seg_top > top)
            top = seg_top;

        // Map the segment to virtual memory
        map_region(proc->user.mem_map, pages, vbase, pbase, flags);

        // Copy all loadable data from the file
        usize offset = vaddr - vbase;
        memcpy(base + offset, (void*)header + p_header->offset, p_header->file_size);

        // Zero out any additional space
        usize zero_len = p_header->mem_size - p_header->file_size;
        memset(base + p_header->file_size, 0, zero_len);
    }

    return top;
}

static void _map_vdso(process* proc) {
    assert(vdso_elf);

    _load_elf_sections(proc, vdso_elf, PROC_VDSO_BASE);

    // Locate the signal_trampoline
    elf_sect_header* dynsym = elf_locate_section(vdso_elf, ".dynsym");
    elf_sect_header* dynstr = elf_locate_section(vdso_elf, ".dynstr");

    assert(dynstr && dynsym);

    elf_symbol* symtab = vdso_elf + dynsym->offset;
    char* strtab = vdso_elf + dynstr->offset;

    elf_symbol* sig_sym = elf_locate_symbol(symtab, dynsym->size, strtab, "signal_trampoline");

    assert(sig_sym);

    proc->user.signals.trampoline = PROC_VDSO_BASE + sig_sym->value;
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

    _load_elf_sections(proc, header, 0);

    // TODO: don't just map to a fixed address
    _init_ustack(proc, PROC_USTACK_BASE, SCHED_USTACK_SIZE);

    // Set the initial state
    int_state state = {
        .s_regs.rip = (u64)header->entry,
        .s_regs.cs = GDT_user_code | 3,
        .s_regs.rflags = 0x200,
        .s_regs.rsp = proc->user.stack_vaddr + proc->user.stack_size,
        .s_regs.ss = GDT_user_data | 3,
    };

    process_set_state(proc, &state);

    // Map the vdso to the appropriate address
    // TODO: don't remap if its already mapped
    _map_vdso(proc);

    return true;
}

// TODO: don't just copy everything. Leverage the MMU/page faults
process* process_fork(process* parent) {
    assert(parent->type == PROC_USER); // why?

    usize pid = _next_pid();
    process* child = process_create(parent->name, PROC_USER, pid);

    child->user.tree_entry = tree_create_node(child);
    tree_insert_child(parent->user.tree_entry, child->user.tree_entry);

    process_init_stack(parent, child);
    process_init_page_map(parent, child);
    process_init_file_descriptors(parent, child);
    process_init_signal_handlers(parent, child);

    return child;
}


void load_vdso() {
    vfs_node* file = vfs_lookup_relative(INITRD_MOUNT, "usr/vdso.elf");

    if (!file)
        panic("vdso.elf not found!");

    vdso_elf = kmalloc(file->size);
    file->interface->read(file, vdso_elf, 0, file->size);

    if (elf_verify(vdso_elf) != VALID_ELF)
        panic("vdso.elf is invalid!");
}
