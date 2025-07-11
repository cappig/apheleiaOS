#include "exec.h"

#include <base/addr.h>
#include <base/macros.h>
#include <base/types.h>
#include <errno.h>
#include <parse/elf.h>
#include <string.h>
#include <x86/paging.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "data/vector.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "mem/virtual.h"
#include "process.h"
#include "sys/panic.h"


static bool _init_ustack(sched_thread* thread, u64 base, usize pages) {
    if (thread->ustack.vaddr)
        return false;

    sched_process* proc = thread->proc;

    thread->ustack.vaddr = base;

    thread->ustack.size = pages * PAGE_4KIB;
    thread->ustack.paddr = (u64)alloc_frames(pages);

    u64 flags = PT_PRESENT | PT_NO_EXECUTE | PT_WRITE | PT_USER;
    map_region(proc->memory.table, pages, thread->ustack.vaddr, thread->ustack.paddr, flags);

    memory_region new_region = {
        .base = base,
        .size = thread->ustack.size,

        .flags = MAP_PRIVATE | MAP_FIXED,
        .prot = PROT_WRITE | PROT_READ,
    };

    proc_insert_mem_region(proc, &new_region);

    // Writes to this page will fault, and we can go from there
    // map_page(proc->memory.table, PAGE_4KIB, base, 0, PT_NO_EXECUTE);
    // TODO: map a dummy canary page at the bottom of the stack to detect overflow

    return true;
}

static bool
_load_elf_sections(sched_thread* thread, vfs_node* file, elf_header* header, usize load_offset) {
    sched_process* proc = thread->proc;

    elf_prog_header* p_header = kmalloc(header->phent_size);

    bool success = true;

    for (usize i = 0; i < header->ph_num; i++) {
        usize header_offset = header->phoff + i * header->phent_size;

        if (vfs_read(file, p_header, header_offset, header->phent_size, 0) < 0)
            break;

        if (p_header->type != PT_LOAD)
            continue;

        if (!p_header->file_size && !p_header->mem_size)
            continue;

        memory_region region = {0};

        region.base = p_header->vaddr + load_offset;

        u64 vbase = ALIGN_DOWN(region.base, PAGE_4KIB);
        u64 vend = ALIGN(region.base + p_header->mem_size, PAGE_4KIB);

        region.size = vend - vbase;
        usize pages = region.size / PAGE_4KIB;

        u64 pbase = (u64)alloc_frames(pages);
        void* base = (void*)ID_MAPPED_VADDR(pbase);

        region.offset = region.base - vbase;
        region.file = file;

        u64 page_flags = elf_to_page_flags(p_header->flags) | PT_USER;

        region.flags = MAP_PRIVATE | MAP_FIXED;
        region.prot = page_flags_to_prot(page_flags);

        map_region(proc->memory.table, pages, vbase, pbase, page_flags);

        if (vfs_read(file, base + region.offset, p_header->offset, p_header->file_size, 0) < 0) {
            success = false;
            break;
        }

        usize zero_len = p_header->mem_size - p_header->file_size;
        memset(base + region.offset + p_header->file_size, 0, zero_len);

        proc_insert_mem_region(proc, &region);
    }

    kfree(p_header);

    return success;
}

static void _map_vdso(sched_thread* thread) {
    sched_process* proc = thread->proc;

    if (proc->memory.vdso) // the vdso is already loaded
        return;

    vfs_node* file = vfs_lookup("sbin/vdso.elf");

    if (!file)
        panic("vdso.elf not found!");

    elf_header* header = kmalloc(file->size);
    vfs_read(file, header, 0, file->size, 0);

    if (elf_verify(header) != VALID_ELF)
        panic("vdso.elf is invalid!");

    _load_elf_sections(thread, file, header, PROC_VDSO_BASE);

    // Locate the signal_trampoline
    elf_sect_header* dynsym = elf_locate_section(header, ".dynsym");
    elf_sect_header* dynstr = elf_locate_section(header, ".dynstr");

    assert(dynstr && dynsym);

    elf_symbol* symtab = (void*)header + dynsym->offset;
    char* strtab = (void*)header + dynstr->offset;

    elf_symbol* sig_sym = elf_locate_symbol(symtab, dynsym->size, strtab, "signal_trampoline");

    assert(sig_sym);

    proc->memory.vdso = PROC_VDSO_BASE;
    proc->memory.trampoline = PROC_VDSO_BASE + sig_sym->value;

    kfree(header);
}


static void _ustack_push(sched_thread* thread, u64* rsp, u64 qword) {
    *rsp -= sizeof(u64);

    usize offset = *rsp - thread->ustack.vaddr;
    u64 paddr = thread->ustack.paddr + offset;
    u64* vaddr = (void*)ID_MAPPED_VADDR(paddr);

    *vaddr = qword;
}


// We have to allocate new user mapped pages to hold argument strings and environment variables
// returns the final value of the stack pointer
static u64 _alloc_args(sched_thread* thread, char** argv, char** envp) {
    sched_process* proc = thread->proc;

    u64 rsp = thread->ustack.vaddr + thread->ustack.size;

    // How many environment variables do we have
    usize envc = 0;
    while (envp && envp[envc])
        envc++;

    usize argc = 0;
    while (argv && argv[argc])
        argc++;

    if (!argc && !envc) {
        _ustack_push(thread, &rsp, 0);
        _ustack_push(thread, &rsp, 0);
        _ustack_push(thread, &rsp, 0);
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
    u64 user_vbase = thread->ustack.vaddr + thread->ustack.size;

    u64 flags = PT_USER | PT_WRITE | PT_PRESENT;
    map_region(proc->memory.table, pages, user_vbase, pbase, flags);

    usize offset = 0;

    // Push the environment variables, envp
    _ustack_push(thread, &rsp, 0); // terminate the environment variable array

    for (isize i = envc - 1; i >= 0; i--) {
        usize slen = strlen(envp[i]);

        memcpy((char*)vbase + offset, envp[i], slen);

        _ustack_push(thread, &rsp, user_vbase + offset);

        offset += slen + 1;
    }

    // Push the arguments, argv
    _ustack_push(thread, &rsp, 0); // terminate the argument array

    for (isize i = argc - 1; i >= 0; i--) {
        if (!argv[i])
            continue;

        usize slen = strlen(argv[i]);

        memcpy((char*)vbase + offset, argv[i], slen);

        _ustack_push(thread, &rsp, user_vbase + offset);

        offset += slen + 1;
    }

    // Push argc
    _ustack_push(thread, &rsp, argc);

    proc->memory.args_paddr = pbase;
    proc->memory.args_pages = pages;

    return rsp;
}

int exec_elf(sched_thread* thread, vfs_node* file, char** argv, char** envp) {
    if (!file || !thread)
        return -EINVAL;

    sched_process* proc = thread->proc;

    assert(proc->type == PROC_USER);

    vec_clear(proc->memory.regions);

    elf_header header[sizeof(elf_header)] = {0};

    isize read = vfs_read(file, header, 0, sizeof(elf_header), 0);

    if (read < 0)
        return -EFAULT;

    if (elf_verify(header) != VALID_ELF)
        return -EINVAL;

    // TODO: handle ET_DYN
    if (!elf_is_executable(header))
        return -EINVAL;

    if (!_load_elf_sections(thread, file, header, 0))
        return -EFAULT;

    // TODO: don't just map to a fixed address
    _init_ustack(thread, PROC_USTACK_BASE, SCHED_USTACK_PAGES);

    u64 rsp = _alloc_args(thread, argv, envp);

    // Set the initial state
    int_state state = {
        .s_regs.rip = (u64)header->entry,
        .s_regs.cs = GDT_user_code | 3,
        .s_regs.rflags = 0x200,
        .s_regs.rsp = rsp,
        .s_regs.ss = GDT_user_data | 3,
    };

    thread_set_state(thread, &state);

    _map_vdso(thread);

    return 0;
}
