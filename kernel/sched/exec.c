#include <string.h>

#include "arch/gdt.h"
#include "drivers/initrd.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "mem/virtual.h"
#include "process.h"
#include "sys/panic.h"


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
static u64 _load_elf_sections(process* proc, vfs_node* file, elf_header* header, usize load_offset) {
    u64 top = 0;

    elf_prog_header* p_header = kmalloc(header->phent_size);

    for (usize i = 0; i < header->ph_num; i++) {
        usize header_offset = header->phoff + i * header->phent_size;

        if (vfs_read(file, p_header, header_offset, header->phent_size) < 0)
            break;

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

        if (vfs_read(file, base + offset, p_header->offset, p_header->file_size) < 0)
            break;

        // Zero out any additional space
        usize zero_len = p_header->mem_size - p_header->file_size;
        memset(base + p_header->file_size, 0, zero_len);
    }

    kfree(p_header);

    return top;
}

static void _map_vdso(process* proc) {
    if (proc->user.vdso) // the vdso is already loaded
        return;

    vfs_node* file = vfs_lookup_relative(INITRD_MOUNT, "usr/vdso.elf");

    if (!file)
        panic("vdso.elf not found!");

    elf_header* header = kmalloc(file->size);
    vfs_read(file, header, 0, file->size);

    if (elf_verify(header) != VALID_ELF)
        panic("vdso.elf is invalid!");

    _load_elf_sections(proc, file, header, PROC_VDSO_BASE);

    // Locate the signal_trampoline
    elf_sect_header* dynsym = elf_locate_section(header, ".dynsym");
    elf_sect_header* dynstr = elf_locate_section(header, ".dynstr");

    assert(dynstr && dynsym);

    elf_symbol* symtab = (void*)header + dynsym->offset;
    char* strtab = (void*)header + dynstr->offset;

    elf_symbol* sig_sym = elf_locate_symbol(symtab, dynsym->size, strtab, "signal_trampoline");

    assert(sig_sym);

    proc->user.vdso = (void*)PROC_VDSO_BASE;
    proc->user.signals.trampoline = PROC_VDSO_BASE + sig_sym->value;

    kfree(header);
}


static void _ustack_push(process* proc, u64* rsp, u64 qword) {
    *rsp -= sizeof(u64);

    usize offset = *rsp - proc->user.stack_vaddr;
    u64 paddr = proc->user.stack_paddr + offset;
    u64* vaddr = (void*)ID_MAPPED_VADDR(paddr);

    *vaddr = qword;
}


// We have to allocate new user mapped pages to hold argument strings and environment variables
// returns the final value of the stack pointer
static u64 _alloc_args(process* proc, char** argv, char** envp) {
    u64 rsp = proc->user.stack_vaddr + proc->user.stack_size;

    // How many environment variables do we have
    usize envc = 0;
    while (envp && envp[envc])
        envc++;

    usize argc = 0;
    while (argv && argv[argc])
        argc++;


    if (!argc && !envc) {
        _ustack_push(proc, &rsp, 0);
        _ustack_push(proc, &rsp, 0);
        _ustack_push(proc, &rsp, 0);
        return rsp;
    }

    // How much space do we need to store the strings
    usize len = 0;

    for (usize i = 0; i < envc; i++)
        len += strlen(envp[i]);

    for (usize i = 0; i < argc; i++)
        len += strlen(argv[i]);

    usize pages = DIV_ROUND_UP(len, PAGE_4KIB);
    u64 pbase = (u64)alloc_frames(pages);
    u64 vbase = ID_MAPPED_VADDR(pbase);

    // Place the pages right abbove the userspace stack
    u64 user_vbase = proc->user.stack_vaddr + proc->user.stack_size;

    u64 flags = PT_USER | PT_WRITE | PT_PRESENT;
    map_region(proc->user.mem_map, pages, user_vbase, pbase, flags);

    usize offset = 0;

    // Push the environment variables, envp
    _ustack_push(proc, &rsp, 0); // terminate the environment variable array

    for (isize i = envc - 1; i >= 0; i--) {
        usize slen = strlen(envp[i]);

        memcpy((char*)vbase + offset, envp[i], slen);

        _ustack_push(proc, &rsp, user_vbase + offset);

        offset += slen + 1;
    }

    // Push the arguments, argv
    _ustack_push(proc, &rsp, 0); // terminate the argument array

    for (isize i = argc - 1; i >= 0; i--) {
        if (!argv[i])
            continue;

        usize slen = strlen(argv[i]);

        memcpy((char*)vbase + offset, argv[i], slen);

        _ustack_push(proc, &rsp, user_vbase + offset);

        offset += slen + 1;
    }

    // Push argc
    _ustack_push(proc, &rsp, argc);

    proc->user.args_paddr = pbase;
    proc->user.args_pages = pages;

    return rsp;
}


bool exec_elf(process* proc, vfs_node* file, char** argv, char** envp) {
    assert(proc->type == PROC_USER);

    if (!file || !proc)
        return false;

    elf_header header[sizeof(elf_header)] = {0};
    // elf_header* header = kmalloc(sizeof(elf_header));

    vfs_read(file, header, 0, sizeof(elf_header));

    if (elf_verify(header) != VALID_ELF)
        return false;

    // TODO: handle ET_DYN
    if (!elf_is_executable(header))
        return false;

    _load_elf_sections(proc, file, header, 0);

    // TODO: don't just map to a fixed address
    _init_ustack(proc, PROC_USTACK_BASE, SCHED_USTACK_SIZE);

    u64 rsp = _alloc_args(proc, argv, envp);

    // Set the initial state
    int_state state = {
        .s_regs.rip = (u64)header->entry,
        .s_regs.cs = GDT_user_code | 3,
        .s_regs.rflags = 0x200,
        .s_regs.rsp = rsp,
        .s_regs.ss = GDT_user_data | 3,
    };

    process_set_state(proc, &state);

    // Map the vdso to the appropriate address
    _map_vdso(proc);

    return true;
}
