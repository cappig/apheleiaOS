#include "exec.h"

#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <log/log.h>
#include <parse/elf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>

#define USER_STACK_PAGES 16

static uintptr_t next_stack_top;

typedef struct PACKED {
    u32 magic;
    u8 arch;
    u8 endianness;
    u8 id_version;
    u8 abi;
    u8 abi_version;
    u8 _unused0[7];

    u16 type;
    u16 machine;
    u32 version;

    u32 entry;

    u32 phoff;
    u32 shoff;

    u32 flags;
    u16 hdr_size;

    u16 phent_size;
    u16 ph_num;

    u16 shdr_size;
    u16 sh_num;

    u16 shstrndx;
} elf32_header_t;

typedef struct PACKED {
    u32 type;
    u32 offset;
    u32 vaddr;
    u32 paddr;
    u32 file_size;
    u32 mem_size;
    u32 flags;
    u32 align;
} elf32_prog_header_t;

static u64 _elf_flags_to_page_flags(u32 elf_flags) {
    u64 flags = PT_USER;

    if (elf_flags & PF_W)
        flags |= PT_WRITE;

    if (arch_supports_nx() && !(elf_flags & PF_X))
        flags |= PT_NO_EXECUTE;

    return flags;
}

static bool
_map_user_region(sched_thread_t* thread, uintptr_t vaddr, uintptr_t paddr, size_t pages, u64 flags) {
    if (!thread || !thread->vm_space || !pages)
        return false;

    void* root = arch_vm_root(thread->vm_space);
    if (!root)
        return false;

    arch_map_region(root, pages, vaddr, paddr, flags);
    return sched_add_user_region(thread, vaddr, paddr, pages, flags);
}

static bool _load_segments_64(sched_thread_t* thread, const u8* image, size_t size, u64* entry_out) {
    if (size < sizeof(elf_header_t))
        return false;

    const elf_header_t* header = (const elf_header_t*)image;

    if (header->magic != ELF_MAGIC || header->arch != EARCH_64)
        return false;

    if (header->phoff + (u64)header->ph_num * header->phent_size > size)
        return false;

    const elf_prog_header_t* prog = (const elf_prog_header_t*)(image + header->phoff);

    for (size_t i = 0; i < header->ph_num; i++) {
        const elf_prog_header_t* ph =
            (const elf_prog_header_t*)((const u8*)prog + i * header->phent_size);

        if (ph->type != PT_LOAD)
            continue;

        if (!ph->mem_size)
            continue;

        u64 map_base = ALIGN_DOWN(ph->vaddr, PAGE_4KIB);
        u64 map_end = ALIGN(ph->vaddr + ph->mem_size, PAGE_4KIB);
        size_t pages = (size_t)((map_end - map_base) / PAGE_4KIB);

        u64 flags = _elf_flags_to_page_flags(ph->flags);
        // Allow loading into read-only segments during early bring-up.
        flags |= PT_WRITE;

        uintptr_t paddr = (uintptr_t)arch_alloc_frames_user(pages);

        if (!_map_user_region(thread, map_base, paddr, pages, flags))
            return false;

        size_t copy_off = (size_t)(ph->vaddr - map_base);
        size_t copy_len = (size_t)ph->file_size;

        void* dst = arch_phys_map(paddr, pages * PAGE_4KIB);
        if (!dst)
            return false;

        memset(dst, 0, pages * PAGE_4KIB);

        if (ph->offset + copy_len <= size)
            memcpy((u8*)dst + copy_off, image + ph->offset, copy_len);

        arch_phys_unmap(dst, pages * PAGE_4KIB);
    }

    if (entry_out)
        *entry_out = header->entry;

    return true;
}

static bool _load_segments_32(sched_thread_t* thread, const u8* image, size_t size, u32* entry_out) {
    if (size < sizeof(elf32_header_t))
        return false;

    const elf32_header_t* header = (const elf32_header_t*)image;

    if (header->magic != ELF_MAGIC || header->arch != EARCH_32)
        return false;

    if (header->phoff + (u32)header->ph_num * header->phent_size > size)
        return false;

    const elf32_prog_header_t* prog = (const elf32_prog_header_t*)(image + header->phoff);

    for (size_t i = 0; i < header->ph_num; i++) {
        const elf32_prog_header_t* ph =
            (const elf32_prog_header_t*)((const u8*)prog + i * header->phent_size);

        if (ph->type != PT_LOAD)
            continue;

        if (!ph->mem_size)
            continue;

        u32 map_base = ALIGN_DOWN(ph->vaddr, PAGE_4KIB);
        u32 map_end = ALIGN(ph->vaddr + ph->mem_size, PAGE_4KIB);
        size_t pages = (size_t)((map_end - map_base) / PAGE_4KIB);

        u64 flags = _elf_flags_to_page_flags(ph->flags);

        uintptr_t paddr = (uintptr_t)arch_alloc_frames_user(pages);

        if (!_map_user_region(thread, map_base, paddr, pages, flags))
            return false;

        size_t copy_off = (size_t)(ph->vaddr - map_base);
        size_t copy_len = (size_t)ph->file_size;

        void* dst = arch_phys_map(paddr, pages * PAGE_4KIB);
        if (!dst)
            return false;

        memset(dst, 0, pages * PAGE_4KIB);

        if (ph->offset + copy_len <= size)
            memcpy((u8*)dst + copy_off, image + ph->offset, copy_len);

        arch_phys_unmap(dst, pages * PAGE_4KIB);
    }

    if (entry_out)
        *entry_out = header->entry;

    return true;
}

static bool
_load_user_segments(sched_thread_t* thread, const u8* image, size_t size, arch_word_t* entry_out) {
    if (!thread || !image || size < 16)
        return false;

    u8 arch = ((const elf_header_t*)image)->arch;

    if (arch == EARCH_64) {
        if (!arch_is_64bit())
            return false;

        u64 entry = 0;
        bool ok = _load_segments_64(thread, image, size, &entry);
        if (ok && entry_out)
            *entry_out = (arch_word_t)entry;
        return ok;
    }

    if (arch == EARCH_32) {
        if (arch_is_64bit())
            return false;

        u32 entry = 0;
        bool ok = _load_segments_32(thread, image, size, &entry);
        if (ok && entry_out)
            *entry_out = (arch_word_t)entry;
        return ok;
    }

    return false;
}

static uintptr_t _alloc_stack_base(size_t size) {
    size = ALIGN(size, PAGE_4KIB);

    if (!next_stack_top)
        next_stack_top = (uintptr_t)arch_user_stack_top();

    if (next_stack_top <= size)
        return 0;

    next_stack_top = ALIGN_DOWN(next_stack_top - size, PAGE_4KIB);
    return next_stack_top;
}

static bool _map_user_stack(sched_thread_t* thread, uintptr_t* stack_top_out) {
    if (!thread || !stack_top_out)
        return false;

    size_t pages = USER_STACK_PAGES;
    size_t size = pages * PAGE_4KIB;

    uintptr_t base = thread->user_stack_base;
    if (!base)
        base = _alloc_stack_base(size);

    if (!base)
        return false;

    uintptr_t paddr = (uintptr_t)arch_alloc_frames_user(pages);

    u64 flags = arch_user_stack_flags();
    if (!map_user_region(thread, base, paddr, pages, flags))
        return false;

    thread->user_stack_base = base;
    thread->user_stack_size = size;
    *stack_top_out = base + size;

    return true;
}

static bool _read_file(vfs_node_t* node, u8** buffer_out, size_t* size_out) {
    if (!node || !buffer_out || !size_out)
        return false;

    size_t size = (size_t)node->size;
    if (!size)
        return false;

    u8* buffer = malloc(size);
    if (!buffer)
        return false;

    if (vfs_read(node, buffer, 0, size, 0) < 0) {
        free(buffer);
        return false;
    }

    *buffer_out = buffer;
    *size_out = size;
    return true;
}

static void _free_regions_list(sched_user_region_t* region) {
    while (region) {
        sched_user_region_t* next = region->next;

        if (region->paddr && region->pages)
            arch_free_frames((void*)region->paddr, region->pages);

        free(region);
        region = next;
    }
}

static const char* _basename(const char* path) {
    if (!path)
        return "";

    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool _load_user_image(sched_thread_t* thread, const char* path, uintptr_t* entry_out) {
    if (!thread || !path)
        return false;

    vfs_node_t* node = vfs_lookup(path);
    if (!node) {
        log_warn("exec: '%s' not found", path);
        return false;
    }

    if (node->type != VFS_FILE) {
        log_warn("exec: '%s' is not a file (type=%u)", path, node->type);
        return false;
    }

    u8* buffer = NULL;
    size_t size = 0;

    if (!_read_file(node, &buffer, &size)) {
        log_warn("exec: failed to read '%s'", path);
        return false;
    }

    bool ok = false;

    arch_word_t entry = 0;
    ok = _load_user_segments(thread, buffer, size, &entry);
    if (ok && entry_out)
        *entry_out = (uintptr_t)entry;

    if (!ok)
        log_warn("exec: '%s' is not a valid executable for this arch", path);

    free(buffer);
    return ok;
}

sched_thread_t* user_spawn(const char* path) {
    if (!path)
        return NULL;

    sched_thread_t* thread = sched_create_user_thread("init");
    if (!thread) {
        log_warn("exec: failed to allocate user thread");
        return NULL;
    }

    arch_vm_space_t* fresh = arch_vm_create_user();
    if (!fresh) {
        log_warn("exec: failed to allocate user address space");
        sched_discard_thread(thread);
        return NULL;
    }

    arch_vm_destroy(thread->vm_space);
    thread->vm_space = fresh;

    uintptr_t entry = 0;
    if (!_load_user_image(thread, path, &entry)) {
        sched_discard_thread(thread);
        return NULL;
    }
    uintptr_t stack_top = 0;
    if (!_map_user_stack(thread, &stack_top)) {
        log_warn("exec: failed to map user stack");
        sched_discard_thread(thread);
        return NULL;
    }

    sched_prepare_user_thread(thread, entry, stack_top);
    thread->ppid = 0;
    sched_set_thread_name(thread, _basename(path));

    thread->state = THREAD_READY;
    return thread;
}

int user_exec(sched_thread_t* thread, const char* path, arch_int_state_t* state) {
    if (!thread || !path)
        return -1;

    arch_vm_space_t* old_vm = thread->vm_space;
    arch_vm_space_t* fresh = arch_vm_create_user();
    if (!fresh)
        return -1;

    sched_user_region_t* old_regions = thread->regions;
    uintptr_t old_stack_base = thread->user_stack_base;
    size_t old_stack_size = thread->user_stack_size;

    thread->vm_space = fresh;
    thread->regions = NULL;
    thread->user_stack_base = old_stack_base;
    thread->user_stack_size = old_stack_size;

    uintptr_t entry_point = 0;
    if (!_load_user_image(thread, path, &entry_point)) {
        sched_clear_user_regions(thread);
        thread->regions = old_regions;
        thread->vm_space = old_vm;
        arch_vm_destroy(fresh);
        return -1;
    }

    uintptr_t stack_top = 0;
    if (!_map_user_stack(thread, &stack_top)) {
        _free_regions_list(thread->regions);
        thread->regions = old_regions;
        thread->vm_space = old_vm;
        arch_vm_destroy(fresh);
        return -1;
    }

    arch_vm_switch(thread->vm_space);
    if (old_vm && old_vm != arch_vm_kernel())
        arch_vm_destroy(old_vm);

    if (old_regions)
        _free_regions_list(old_regions);

    sched_prepare_user_thread(thread, entry_point, stack_top);
    if (state) {
        memset(state, 0, sizeof(*state));
        arch_state_set_user_entry(state, entry_point, stack_top);
        arch_state_set_return(state, 0);
    }

    if (args.argv[0])
        sched_set_thread_name(thread, _basename(args.argv[0]));
    else
        sched_set_thread_name(thread, _basename(path));

    exec_free_args(&args);
    return 0;
}
