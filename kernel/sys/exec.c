#include "exec.h"

#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <log/log.h>
#include <parse/elf.h>
#include <sched/signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/path.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#define EXEC_PROBE_SIZE 512

typedef struct {
    uintptr_t next_top;
    spinlock_t lock;
} stack_alloc_t;

static stack_alloc_t stack_alloc = {
    .lock = SPINLOCK_INIT,
};

typedef struct {
    int argc;
    size_t bytes;
    char *argv[EXEC_MAX_ARGS + 1];
} exec_args_t;

typedef struct {
    int envc;
    size_t bytes;
    char *envp[EXEC_MAX_ENV + 1];
} exec_env_t;

typedef struct {
    char path[PATH_MAX];
    char arg[EXEC_MAX_ARG_LEN];
    bool has_arg;
} exec_shebang_t;

typedef struct {
    char resolved[PATH_MAX];
    vfs_node_t *node;
    u8 probe[EXEC_PROBE_SIZE];
    size_t probe_size;
    size_t size;
} exec_file_t;

typedef struct exec_loaded_page {
    uintptr_t vaddr;
    uintptr_t paddr;
    u64 flags;
    struct exec_loaded_page *next;
} exec_loaded_page_t;

static u64 _elf_page_flags(u32 elf_flags) {
    u64 flags = PT_USER;

    if (elf_flags & PF_W) {
        flags |= PT_WRITE;
    }

    if (arch_supports_nx() && !(elf_flags & PF_X)) {
        flags |= PT_NO_EXECUTE;
    }

    return flags;
}

static bool _u64_add(u64 a, u64 b, u64 *out) {
    if (!out || b > UINT64_MAX - a) {
        return false;
    }

    *out = a + b;
    return true;
}

static bool _u64_mul(u64 a, u64 b, u64 *out) {
    if (!out || (a && b > UINT64_MAX / a)) {
        return false;
    }

    *out = a * b;
    return true;
}

static bool _phdr_table_ok(u64 phoff, u64 ph_num, u64 phent_size, size_t image_size, size_t min_entry_size) {
    if (phent_size < min_entry_size) {
        return false;
    }

    u64 ph_bytes = 0;
    u64 ph_end = 0;

    if (!_u64_mul(ph_num, phent_size, &ph_bytes)) {
        return false;
    }

    if (!_u64_add(phoff, ph_bytes, &ph_end)) {
        return false;
    }

    return phoff <= image_size && ph_end <= image_size;
}

static bool _elf_segment_ok(u64 file_size, u64 mem_size, u64 offset, u64 image_size, u64 vaddr) {
    uintptr_t user_top = (uintptr_t)arch_user_stack_top();
    u64 mem_end = 0;

    if (!mem_size) {
        return false;
    }

    if (!user_top || vaddr < PAGE_4KIB || vaddr >= (u64)user_top) {
        return false;
    }

    if (file_size > mem_size) {
        return false;
    }

    if (offset > image_size || file_size > image_size - offset) {
        return false;
    }

    if (!_u64_add(vaddr, mem_size, &mem_end)) {
        return false;
    }

    if (mem_end > (u64)user_top) {
        return false;
    }

    return true;
}

static bool _segment_range(u64 vaddr, u64 mem_size, u64 max_addr, u64 *base_out, u64 *end_out) {
    if (!base_out || !end_out || max_addr < PAGE_4KIB - 1) {
        return false;
    }

    // keep this in u64 until the range is proven sane; 32-bit ELFs can wrap too
    u64 mem_end = 0;
    if (!_u64_add(vaddr, mem_size, &mem_end)) {
        return false;
    }

    if (mem_end > max_addr - (PAGE_4KIB - 1)) {
        return false;
    }

    u64 base = ALIGN_DOWN(vaddr, PAGE_4KIB);
    u64 end = ALIGN(mem_end, PAGE_4KIB);
    if (end <= base) {
        return false;
    }

    *base_out = base;
    *end_out = end;
    return true;
}

static bool _user_region_end(const sched_user_region_t *region, uintptr_t *out) {
    if (!region || !region->pages || !out) {
        return false;
    }

    if (region->pages > SIZE_MAX / PAGE_4KIB) {
        return false;
    }

    uintptr_t size = region->pages * PAGE_4KIB;
    if (size > (uintptr_t)-1 - region->vaddr) {
        return false;
    }

    *out = region->vaddr + size;
    return true;
}

static bool _map_user_region(sched_thread_t *thread, uintptr_t vaddr, uintptr_t paddr, size_t pages, u64 flags) {
    if (!thread || !thread->vm_space || !pages || !paddr) {
        return false;
    }

    void *root = arch_vm_root(thread->vm_space);
    if (!root) {
        arch_free_frames((void *)paddr, pages);
        return false;
    }

    arch_map_region(root, pages, vaddr, paddr, flags);

    if (sched_add_user_region(thread, vaddr, paddr, pages, flags)) {
        return true;
    }

    for (size_t i = 0; i < pages; i++) {
        uintptr_t page = vaddr + i * PAGE_4KIB;
        unmap_page((page_t *)root, page);
        arch_tlb_flush(page);
    }

    arch_free_frames((void *)paddr, pages);
    return false;
}

static sched_user_region_t *find_user_page(sched_thread_t *thread, uintptr_t vaddr) {
    if (!thread) {
        return NULL;
    }

    sched_user_region_t *region = thread->regions;
    while (region) {
        uintptr_t region_end = 0;
        if (_user_region_end(region, &region_end) && vaddr >= region->vaddr && vaddr < region_end) {
            return region;
        }

        region = region->next;
    }

    return NULL;
}

static exec_loaded_page_t *_find_loaded_page(exec_loaded_page_t *pages, uintptr_t vaddr) {
    while (pages) {
        if (pages->vaddr == vaddr) {
            return pages;
        }

        pages = pages->next;
    }

    return NULL;
}

static bool _entry_loaded(exec_loaded_page_t *pages, u64 entry) {
    if (entry > (u64)(uintptr_t)-1 || entry < PAGE_4KIB) {
        return false;
    }

    uintptr_t page_vaddr = ALIGN_DOWN((uintptr_t)entry, PAGE_4KIB);
    return _find_loaded_page(pages, page_vaddr) != NULL;
}

static void _free_loaded_pages(exec_loaded_page_t *pages) {
    while (pages) {
        exec_loaded_page_t *next = pages->next;
        free(pages);
        pages = next;
    }
}

static exec_loaded_page_t *
_ensure_loaded_page(sched_thread_t *thread, exec_loaded_page_t **pages, uintptr_t vaddr, u64 flags) {
    if (!thread || !pages) {
        return NULL;
    }

    exec_loaded_page_t *page = _find_loaded_page(*pages, vaddr);
    if (page) {
        u64 merged = page->flags | flags;
        if (merged != page->flags) {
            page_t *root = arch_vm_root(thread->vm_space);
            if (!root) {
                return NULL;
            }

            page->flags = merged;
            arch_map_region(root, 1, vaddr, page->paddr, merged);
            arch_tlb_flush(vaddr);

            sched_user_region_t *region = find_user_page(thread, vaddr);
            if (region && region->pages == 1 && region->vaddr == vaddr) {
                region->flags = merged;
            }
        }

        return page;
    }

    uintptr_t paddr = (uintptr_t)arch_alloc_frames_user(1);
    if (!paddr) {
        return NULL;
    }

    if (!_map_user_region(thread, vaddr, paddr, 1, flags)) {
        return NULL;
    }

    void *dst = arch_phys_map(paddr, PAGE_4KIB, 0);
    if (!dst) {
        return NULL;
    }

    memset(dst, 0, PAGE_4KIB);
    arch_phys_unmap(dst, PAGE_4KIB);

    page = calloc(1, sizeof(*page));
    if (!page) {
        return NULL;
    }

    page->vaddr = vaddr;
    page->paddr = paddr;
    page->flags = flags;
    page->next = *pages;
    *pages = page;
    return page;
}

static bool _read_exact(vfs_node_t *node, void *buf, size_t offset, size_t len) {
    if (!len) {
        return true;
    }

    if (!node || !buf || node->size > SIZE_MAX) {
        return false;
    }

    size_t size = (size_t)node->size;
    if (offset > size || len > size - offset) {
        return false;
    }

    return vfs_read(node, buf, offset, len, 0) == (ssize_t)len;
}

typedef struct {
    vfs_node_t *node;
    size_t image_size;
    u64 phoff;
    u64 ph_num;
    u64 phent_size;
    size_t min_entry_size;
    u8 **out;
} phdr_read_t;

static bool _read_phdrs(const phdr_read_t *read) {
    if (!read || !read->out) {
        return false;
    }

    *read->out = NULL;

    if (!_phdr_table_ok(read->phoff, read->ph_num, read->phent_size, read->image_size, read->min_entry_size)) {
        return false;
    }

    u64 ph_bytes = 0;
    if (!_u64_mul(read->ph_num, read->phent_size, &ph_bytes) || !ph_bytes || ph_bytes > SIZE_MAX) {
        return false;
    }

    u8 *table = malloc((size_t)ph_bytes);
    if (!table) {
        return false;
    }

    if (!_read_exact(read->node, table, (size_t)read->phoff, (size_t)ph_bytes)) {
        free(table);
        return false;
    }

    *read->out = table;
    return true;
}

static bool copy_segment(
    vfs_node_t *node,
    size_t image_size,
    u64 offset,
    u64 file_size,
    uintptr_t vaddr,
    exec_loaded_page_t *pages
) {
    if (!file_size) {
        return true;
    }

    bool out_of_range = offset > image_size || file_size > image_size - offset;

    if (!node || out_of_range || file_size > SIZE_MAX) {
        return false;
    }

    size_t remaining = (size_t)file_size;
    size_t image_off = (size_t)offset;
    uintptr_t cursor = vaddr;
    u8 *bounce = NULL;

    if (!arch_keeps_phys_map()) {
        bounce = malloc(PAGE_4KIB);
        if (!bounce) {
            return false;
        }
    }

    while (remaining) {
        uintptr_t page_vaddr = ALIGN_DOWN(cursor, PAGE_4KIB);
        exec_loaded_page_t *page = _find_loaded_page(pages, page_vaddr);
        if (!page) {
            free(bounce);
            return false;
        }

        size_t page_off = (size_t)(cursor - page_vaddr);
        size_t chunk = PAGE_4KIB - page_off;
        if (chunk > remaining) {
            chunk = remaining;
        }

        if (bounce && !_read_exact(node, bounce, image_off, chunk)) {
            free(bounce);
            return false;
        }

        void *dst = arch_phys_map(page->paddr, PAGE_4KIB, 0);
        if (!dst) {
            free(bounce);
            return false;
        }

        bool copied = true;
        if (bounce) {
            memcpy((u8 *)dst + page_off, bounce, chunk);
        } else {
            copied = _read_exact(node, (u8 *)dst + page_off, image_off, chunk);
        }

        arch_phys_unmap(dst, PAGE_4KIB);

        if (!copied) {
            free(bounce);
            return false;
        }

        cursor += chunk;
        image_off += chunk;
        remaining -= chunk;
    }

    free(bounce);
    return true;
}

static bool _load_segments_64(sched_thread_t *thread, const exec_file_t *file, u64 *entry_out) {
    if (!file || file->probe_size < sizeof(elf_header_t)) {
        return false;
    }

    elf_header_t header;
    memcpy(&header, file->probe, sizeof(header));

    if (header.magic != ELF_MAGIC || header.arch != EARCH_64) {
        return false;
    }

    u8 *phdrs = NULL;
    phdr_read_t read = {
        .node = file->node,
        .image_size = file->size,
        .phoff = header.phoff,
        .ph_num = header.ph_num,
        .phent_size = header.phent_size,
        .min_entry_size = sizeof(elf_prog_header_t),
        .out = &phdrs,
    };

    if (!_read_phdrs(&read)) {
        return false;
    }

    exec_loaded_page_t *loaded_pages = NULL;
    bool loaded = false;

    for (size_t i = 0; i < header.ph_num; i++) {
        const u8 *ph_ptr = phdrs + i * header.phent_size;
        const elf_prog_header_t *ph = (const elf_prog_header_t *)ph_ptr;

        if (ph->type != PT_LOAD) {
            continue;
        }

        if (!ph->mem_size) {
            continue;
        }

        bool segment_ok = _elf_segment_ok(ph->file_size, ph->mem_size, ph->offset, file->size, ph->vaddr);

        if (!segment_ok) {
            goto out;
        }

        u64 map_base = 0;
        u64 map_end = 0;
        if (!_segment_range(ph->vaddr, ph->mem_size, UINT64_MAX, &map_base, &map_end)) {
            goto out;
        }

        u64 flags = _elf_page_flags(ph->flags);
        for (u64 page_vaddr = map_base; page_vaddr < map_end; page_vaddr += PAGE_4KIB) {
            if (!_ensure_loaded_page(thread, &loaded_pages, (uintptr_t)page_vaddr, flags)) {
                goto out;
            }
        }

        bool copied = copy_segment(
            file->node,
            file->size,
            ph->offset,
            ph->file_size,
            (uintptr_t)ph->vaddr,
            loaded_pages
        );

        if (!copied) {
            goto out;
        }
    }

    if (!_entry_loaded(loaded_pages, header.entry)) {
        goto out;
    }

    if (entry_out) {
        *entry_out = header.entry;
    }

    loaded = true;

out:
    free(phdrs);
    _free_loaded_pages(loaded_pages);
    return loaded;
}

static bool _load_segments_32(sched_thread_t *thread, const exec_file_t *file, u32 *entry_out) {
    if (!file || file->probe_size < sizeof(elf32_header_t)) {
        return false;
    }

    elf32_header_t header;
    memcpy(&header, file->probe, sizeof(header));

    if (header.magic != ELF_MAGIC || header.arch != EARCH_32) {
        return false;
    }

    u8 *phdrs = NULL;
    phdr_read_t read = {
        .node = file->node,
        .image_size = file->size,
        .phoff = header.phoff,
        .ph_num = header.ph_num,
        .phent_size = header.phent_size,
        .min_entry_size = sizeof(elf32_prog_header_t),
        .out = &phdrs,
    };

    if (!_read_phdrs(&read)) {
        return false;
    }

    exec_loaded_page_t *loaded_pages = NULL;
    bool loaded = false;

    for (size_t i = 0; i < header.ph_num; i++) {
        const u8 *ph_ptr = phdrs + i * header.phent_size;
        const elf32_prog_header_t *ph = (const elf32_prog_header_t *)ph_ptr;

        if (ph->type != PT_LOAD) {
            continue;
        }

        if (!ph->mem_size) {
            continue;
        }

        bool segment_ok = _elf_segment_ok(ph->file_size, ph->mem_size, ph->offset, file->size, ph->vaddr);

        if (!segment_ok) {
            goto out;
        }

        u64 map_base = 0;
        u64 map_end = 0;
        if (!_segment_range(ph->vaddr, ph->mem_size, UINT32_MAX, &map_base, &map_end)) {
            goto out;
        }

        u64 flags = _elf_page_flags(ph->flags);
        for (u64 page_vaddr = map_base; page_vaddr < map_end; page_vaddr += PAGE_4KIB) {
            if (!_ensure_loaded_page(thread, &loaded_pages, (uintptr_t)page_vaddr, flags)) {
                goto out;
            }
        }

        bool copied = copy_segment(
            file->node,
            file->size,
            ph->offset,
            ph->file_size,
            (uintptr_t)ph->vaddr,
            loaded_pages
        );

        if (!copied) {
            goto out;
        }
    }

    if (!_entry_loaded(loaded_pages, header.entry)) {
        goto out;
    }

    if (entry_out) {
        *entry_out = header.entry;
    }

    loaded = true;

out:
    free(phdrs);
    _free_loaded_pages(loaded_pages);
    return loaded;
}

static bool _load_user_segments(sched_thread_t *thread, const exec_file_t *file, arch_word_t *entry_out) {
    if (!thread || !file || file->probe_size < 16) {
        return false;
    }

    u8 arch = file->probe[4];

    if (arch == EARCH_64) {
        if (!arch_is_64bit()) {
            return false;
        }

        u64 entry = 0;
        bool loaded = _load_segments_64(thread, file, &entry);

        if (loaded && entry_out) {
            *entry_out = (arch_word_t)entry;
        }

        return loaded;
    }

    if (arch == EARCH_32) {
        if (arch_is_64bit()) {
            return false;
        }

        u32 entry = 0;
        bool loaded = _load_segments_32(thread, file, &entry);

        if (loaded && entry_out) {
            *entry_out = (arch_word_t)entry;
        }

        return loaded;
    }

    return false;
}

static uintptr_t _alloc_stack_base(size_t size) {
    size = ALIGN(size, PAGE_4KIB);

    unsigned long irq_flags = spin_lock_irqsave(&stack_alloc.lock);

    if (!stack_alloc.next_top) {
        stack_alloc.next_top = (uintptr_t)arch_user_stack_top();
    }

    uintptr_t base = 0;

    if (stack_alloc.next_top > size) {
        stack_alloc.next_top = ALIGN_DOWN(stack_alloc.next_top - size, PAGE_4KIB);
        base = stack_alloc.next_top;
    }

    spin_unlock_irqrestore(&stack_alloc.lock, irq_flags);
    return base;
}

static bool _exec_bytes_ok(size_t arg_bytes, size_t env_bytes) {
    return arg_bytes <= EXEC_ARG_MAX && env_bytes <= EXEC_ARG_MAX - arg_bytes;
}

static bool _exec_add_ok(size_t base, size_t add, size_t *out) {
    if (!out || add > SIZE_MAX - base) {
        return false;
    }

    *out = base + add;
    return true;
}

static int _args_push(exec_args_t *args, const char *value) {
    if (!args || !value) {
        return -EINVAL;
    }

    if (args->argc >= EXEC_MAX_ARGS) {
        return -E2BIG;
    }

    size_t len = strnlen(value, EXEC_MAX_ARG_LEN);
    if (len >= EXEC_MAX_ARG_LEN) {
        return -E2BIG;
    }

    size_t bytes = len + 1 + sizeof(char *);
    size_t next_bytes = 0;

    if (!_exec_add_ok(args->bytes, bytes, &next_bytes) || !_exec_bytes_ok(next_bytes, 0)) {
        return -E2BIG;
    }

    char *copy = malloc(len + 1);
    if (!copy) {
        return -ENOMEM;
    }

    memcpy(copy, value, len);
    copy[len] = '\0';

    args->argv[args->argc++] = copy;
    args->argv[args->argc] = NULL;
    args->bytes = next_bytes;

    return 0;
}

static void _free_args(exec_args_t *args) {
    if (!args) {
        return;
    }

    for (int i = 0; i < args->argc; i++) {
        free(args->argv[i]);
        args->argv[i] = NULL;
    }

    args->argc = 0;
    args->bytes = 0;
    args->argv[0] = NULL;
}

static int _env_push(exec_env_t *env, const char *value) {
    if (!env || !value) {
        return -EINVAL;
    }

    if (env->envc >= EXEC_MAX_ENV) {
        return -E2BIG;
    }

    size_t len = strnlen(value, EXEC_MAX_ENV_LEN);

    if (len >= EXEC_MAX_ENV_LEN) {
        return -E2BIG;
    }

    size_t bytes = len + 1 + sizeof(char *);
    size_t next_bytes = 0;

    if (!_exec_add_ok(env->bytes, bytes, &next_bytes) || !_exec_bytes_ok(next_bytes, 0)) {
        return -E2BIG;
    }

    char *copy = malloc(len + 1);
    if (!copy) {
        return -ENOMEM;
    }

    memcpy(copy, value, len);
    copy[len] = '\0';

    env->envp[env->envc++] = copy;
    env->envp[env->envc] = NULL;
    env->bytes = next_bytes;

    return 0;
}

static void _free_env(exec_env_t *env) {
    if (!env) {
        return;
    }

    for (int i = 0; i < env->envc; i++) {
        free(env->envp[i]);
        env->envp[i] = NULL;
    }

    env->envc = 0;
    env->bytes = 0;
    env->envp[0] = NULL;
}

static int _copy_args(char *const argv[], exec_args_t *out) {
    if (!out) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));

    if (!argv) {
        return 0;
    }

    for (int i = 0; i < EXEC_MAX_ARGS; i++) {
        const char *arg = argv[i];
        if (!arg) {
            break;
        }

        int err = _args_push(out, arg);
        if (err) {
            _free_args(out);
            return err;
        }
    }

    if (argv[EXEC_MAX_ARGS]) {
        _free_args(out);
        return -E2BIG;
    }

    out->argv[out->argc] = NULL;
    return 0;
}

static int _copy_env(char *const envp[], exec_env_t *out) {
    if (!out) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));

    if (!envp) {
        return 0;
    }

    for (int i = 0; i < EXEC_MAX_ENV; i++) {
        const char *env = envp[i];
        if (!env) {
            break;
        }

        int err = _env_push(out, env);
        if (err) {
            _free_env(out);
            return err;
        }
    }

    if (envp[EXEC_MAX_ENV]) {
        _free_env(out);
        return -E2BIG;
    }

    out->envp[out->envc] = NULL;
    return 0;
}

static uintptr_t build_stack_args(uintptr_t stack_top, const exec_args_t *args, const exec_env_t *env) {
    uintptr_t sp = stack_top;

    size_t argc = args ? (size_t)args->argc : 0;
    size_t envc = env ? (size_t)env->envc : 0;

    uintptr_t arg_ptrs[EXEC_MAX_ARGS] = { 0 };
    uintptr_t env_ptrs[EXEC_MAX_ENV] = { 0 };

    // strings are copied first so the pointer tables can be written last
    for (int i = (int)envc - 1; i >= 0; i--) {
        size_t len = strlen(env->envp[i]) + 1;
        sp -= len;
        memcpy((void *)sp, env->envp[i], len);
        env_ptrs[i] = sp;
    }

    for (int i = (int)argc - 1; i >= 0; i--) {
        size_t len = strlen(args->argv[i]) + 1;
        sp -= len;
        memcpy((void *)sp, args->argv[i], len);
        arg_ptrs[i] = sp;
    }

    sp = ALIGN_DOWN(sp, 16);

    size_t slots = argc + envc + 3;
    size_t align_slots = 16 / sizeof(uintptr_t);
    if (!align_slots) {
        align_slots = 1;
    }

    size_t pad_slots = (align_slots - (slots % align_slots)) % align_slots;
    for (size_t i = 0; i < pad_slots; i++) {
        sp -= sizeof(uintptr_t);
        *(uintptr_t *)sp = 0;
    }

    // final stack layout is argc, argv[], null, envp[], null
    sp -= sizeof(uintptr_t);
    *(uintptr_t *)sp = 0;

    for (int i = (int)envc - 1; i >= 0; i--) {
        sp -= sizeof(uintptr_t);
        *(uintptr_t *)sp = env_ptrs[i];
    }

    sp -= sizeof(uintptr_t);
    *(uintptr_t *)sp = 0;

    for (int i = (int)argc - 1; i >= 0; i--) {
        sp -= sizeof(uintptr_t);
        *(uintptr_t *)sp = arg_ptrs[i];
    }

    sp -= sizeof(uintptr_t);
    *(uintptr_t *)sp = (uintptr_t)argc;

    return sp;
}

static bool _map_user_stack(sched_thread_t *thread, uintptr_t *stack_top_out) {
    if (!thread || !stack_top_out) {
        return false;
    }

    size_t pages = USER_STACK_PAGES;
    size_t size = pages * PAGE_4KIB;

    uintptr_t base = thread->user_stack_base;
    if (!base) {
        base = _alloc_stack_base(size);
    }

    if (!base) {
        return false;
    }

    uintptr_t paddr = (uintptr_t)arch_alloc_frames_user(pages);
    if (!paddr) {
        return false;
    }

    u64 flags = arch_user_stack_flags();
    if (!_map_user_region(thread, base, paddr, pages, flags)) {
        return false;
    }

    thread->user_stack_base = base;
    thread->user_stack_size = size;
    *stack_top_out = base + size;

    return true;
}

static void _close_file(exec_file_t *file) {
    if (!file) {
        return;
    }

    vfs_node_release(file->node);
    file->probe_size = 0;
    file->size = 0;
    file->node = NULL;
    file->resolved[0] = '\0';
}

static bool _read_file_probe(exec_file_t *file) {
    if (!file || !file->node) {
        return false;
    }

    if (file->node->size > SIZE_MAX) {
        return false;
    }

    file->size = (size_t)file->node->size;
    if (!file->size) {
        return false;
    }

    size_t probe_size = file->size;
    if (probe_size > sizeof(file->probe)) {
        probe_size = sizeof(file->probe);
    }

    if (!_read_exact(file->node, file->probe, 0, probe_size)) {
        return false;
    }

    file->probe_size = probe_size;
    return true;
}

static int _open_file(sched_thread_t *thread, const char *path, exec_file_t *out, bool require_exec) {
    if (!thread || !path || !out) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));

    if (strlen(path) >= PATH_MAX) {
        return -ENAMETOOLONG;
    }

    bool resolved = path_resolve(thread->cwd, path, out->resolved, sizeof(out->resolved));

    if (!resolved) {
        return -ENOENT;
    }

    int search_err = vfs_check_search(out->resolved, thread->uid, thread->gid, false);

    if (search_err < 0) {
        return search_err;
    }

    out->node = vfs_lookup_hold(out->resolved, true);
    if (!out->node) {
        return -ENOENT;
    }

    if (out->node->type != VFS_FILE) {
        _close_file(out);
        return -EISDIR;
    }

    if (require_exec) {
        int access = vfs_access(out->node, thread->uid, thread->gid, X_OK);
        if (access < 0) {
            _close_file(out);
            return access;
        }
    }

    if (!_read_file_probe(out)) {
        _close_file(out);
        return -EIO;
    }

    return 0;
}

static bool _parse_shebang(const u8 *buffer, size_t size, exec_shebang_t *out) {
    if (!buffer || size < 2 || !out) {
        return false;
    }

    if (buffer[0] != '#' || buffer[1] != '!') {
        return false;
    }

    size_t idx = 2;
    while (idx < size && (buffer[idx] == ' ' || buffer[idx] == '\t')) {
        idx++;
    }

    size_t start = idx;
    while (idx < size) {
        u8 ch = buffer[idx];

        if (ch == '\n' || ch == '\r' || isspace((int)ch)) {
            break;
        }

        idx++;
    }

    size_t len = idx - start;
    if (!len) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (len >= sizeof(out->path)) {
        len = sizeof(out->path) - 1;
    }

    memcpy(out->path, buffer + start, len);
    out->path[len] = '\0';

    while (idx < size && (buffer[idx] == ' ' || buffer[idx] == '\t')) {
        idx++;
    }

    if (idx < size && buffer[idx] != '\n' && buffer[idx] != '\r') {
        start = idx;

        while (idx < size) {
            u8 ch = buffer[idx];

            if (ch == '\n' || ch == '\r' || isspace((int)ch)) {
                break;
            }

            idx++;
        }

        len = idx - start;

        if (len) {
            if (len >= sizeof(out->arg)) {
                len = sizeof(out->arg) - 1;
            }

            memcpy(out->arg, buffer + start, len);

            out->arg[len] = '\0';
            out->has_arg = true;
        }
    }

    return true;
}

static int
_shebang_args(const exec_args_t *orig, const exec_shebang_t *shebang, const char *script_path, exec_args_t *out) {
    if (!shebang || !script_path || !out) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));

    int err = _args_push(out, shebang->path);
    if (err) {
        _free_args(out);
        return err;
    }

    if (shebang->has_arg) {
        err = _args_push(out, shebang->arg);
        if (err) {
            _free_args(out);
            return err;
        }
    }

    err = _args_push(out, script_path);
    if (err) {
        _free_args(out);
        return err;
    }

    if (orig) {
        for (int i = 1; i < orig->argc; i++) {
            err = _args_push(out, orig->argv[i]);
            if (err) {
                _free_args(out);
                return err;
            }
        }
    }

    return 0;
}

static void _free_regions_list(sched_user_region_t *region) {
    while (region) {
        sched_user_region_t *next = region->next;

        if (region->paddr && region->pages) {
            arch_free_frames((void *)region->paddr, region->pages);
        }

        free(region);
        region = next;
    }
}

static const char *_basename(const char *path) {
    if (!path) {
        return "";
    }

    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void apply_identity(sched_thread_t *thread, const vfs_node_t *node) {
    if (!thread || !node) {
        return;
    }

    if (node->mode & S_ISGID) {
        thread->gid = node->gid;
    }

    if (node->mode & S_ISUID) {
        thread->uid = node->uid;
    }

    thread->did_exec = true;
}

sched_thread_t *user_spawn(const char *path) {
    if (!path) {
        return NULL;
    }

    exec_file_t file = { 0 };
    exec_args_t args = { 0 };
    exec_env_t env = { 0 };
    sched_thread_t *result = NULL;

    sched_thread_t *thread = sched_create_user_thread("init");
    if (!thread) {
        log_warn("failed to allocate user thread");
        return NULL;
    }

    arch_vm_space_t *fresh = arch_vm_create_user();
    if (!fresh) {
        log_warn("failed to allocate user address space");
        sched_discard_thread(thread);
        return NULL;
    }

    arch_vm_destroy(thread->vm_space);
    thread->vm_space = fresh;

    int err = _open_file(thread, path, &file, true);
    if (err) {
        goto out;
    }

    char exec_name_buf[PATH_MAX];
    strncpy(exec_name_buf, file.resolved, sizeof(exec_name_buf) - 1);
    exec_name_buf[sizeof(exec_name_buf) - 1] = '\0';

    exec_shebang_t shebang = { 0 };
    bool is_script = _parse_shebang(file.probe, file.probe_size, &shebang);

    if (is_script) {
        err = _shebang_args(NULL, &shebang, exec_name_buf, &args);
        if (err) {
            goto out;
        }

        exec_file_t interp = { 0 };
        err = _open_file(thread, shebang.path, &interp, true);
        if (err) {
            goto out;
        }

        _close_file(&file);
        file = interp;
    } else {
        err = _args_push(&args, path);
        if (err) {
            goto out;
        }
    }

    uintptr_t entry = 0;
    arch_word_t entry_raw = 0;
    bool loaded = _load_user_segments(thread, &file, &entry_raw);
    if (loaded) {
        entry = (uintptr_t)entry_raw;
    }

    if (!loaded) {
        log_warn("'%s' is not a valid executable for this arch", file.resolved);
        goto out;
    }

    uintptr_t stack_top = 0;
    if (!_map_user_stack(thread, &stack_top)) {
        log_warn("failed to map user stack");
        goto out;
    }

    apply_identity(thread, file.node);

    arch_vm_switch(thread->vm_space);
    stack_top = build_stack_args(stack_top, &args, &env);
    arch_code_sync();
    arch_vm_switch(arch_vm_kernel());

    thread_prepare_user(thread, entry, stack_top);
    thread->ppid = 0;
    thread_set_name(thread, _basename(exec_name_buf));

    result = thread;
    thread = NULL;

out:
    _free_args(&args);
    _free_env(&env);
    _close_file(&file);

    if (thread) {
        sched_discard_thread(thread);
    }

    return result;
}

typedef struct {
    arch_vm_space_t *vm;
    sched_user_region_t *regions;
    uintptr_t stack_base;
    size_t stack_size;
    u64 mem_kib;
} exec_image_t;

static exec_image_t exec_take_image(sched_thread_t *thread, arch_vm_space_t *fresh) {
    exec_image_t old = {
        .vm = thread->vm_space,
        .regions = thread->regions,
        .stack_base = thread->user_stack_base,
        .stack_size = thread->user_stack_size,
        .mem_kib = sched_user_mem_kib(thread),
    };

    thread->vm_space = fresh;
    thread->regions = NULL;
    thread->user_stack_base = old.stack_base;
    thread->user_stack_size = old.stack_size;
    sched_set_user_mem(thread, 0);

    return old;
}

static void exec_restore_image(sched_thread_t *thread, arch_vm_space_t *fresh, const exec_image_t *old) {
    sched_clear_user_regions(thread);

    thread->vm_space = old->vm;
    thread->regions = old->regions;
    thread->user_stack_base = old->stack_base;
    thread->user_stack_size = old->stack_size;
    sched_set_user_mem(thread, old->mem_kib);

    arch_vm_destroy(fresh);
}

static void exec_drop_old_image(const exec_image_t *old) {
    if (old->regions) {
        _free_regions_list(old->regions);
    }

    if (old->vm && old->vm != arch_vm_kernel()) {
        arch_vm_destroy(old->vm);
    }
}

typedef struct {
    exec_args_t args;
    exec_env_t env;
    exec_file_t file;
    char name[PATH_MAX];
    bool script;
} exec_plan_t;

static void exec_plan_clear(exec_plan_t *plan) {
    if (!plan) {
        return;
    }

    _free_args(&plan->args);
    _free_env(&plan->env);
    _close_file(&plan->file);
}

static int
exec_plan_load(sched_thread_t *thread, const char *path, char *const argv[], char *const envp[], exec_plan_t *plan) {
    int err = _copy_args(argv, &plan->args);
    if (err) {
        return err;
    }

    err = _copy_env(envp, &plan->env);
    if (err) {
        return err;
    }

    if (!_exec_bytes_ok(plan->args.bytes, plan->env.bytes)) {
        return -E2BIG;
    }

    err = _open_file(thread, path, &plan->file, true);
    if (err) {
        return err;
    }

    strncpy(plan->name, plan->file.resolved, sizeof(plan->name) - 1);
    plan->name[sizeof(plan->name) - 1] = '\0';

    exec_shebang_t shebang = { 0 };
    plan->script = _parse_shebang(plan->file.probe, plan->file.probe_size, &shebang);
    if (!plan->script) {
        return 0;
    }

    exec_args_t script_args = { 0 };
    err = _shebang_args(&plan->args, &shebang, plan->name, &script_args);
    if (err) {
        return err;
    }

    if (!_exec_bytes_ok(script_args.bytes, plan->env.bytes)) {
        _free_args(&script_args);
        return -E2BIG;
    }

    _free_args(&plan->args);
    plan->args = script_args;

    exec_file_t interp = { 0 };
    err = _open_file(thread, shebang.path, &interp, true);
    if (err) {
        return err;
    }

    _close_file(&plan->file);
    plan->file = interp;

    return 0;
}

int user_exec(
    sched_thread_t *thread,
    const char *path,
    char *const argv[],
    char *const envp[],
    arch_int_state_t *state
) {
    if (!thread || !path) {
        return -EINVAL;
    }

    exec_plan_t plan = { 0 };
    int err = exec_plan_load(thread, path, argv, envp, &plan);
    if (err) {
        goto out;
    }

    arch_vm_space_t *fresh = arch_vm_create_user();
    if (!fresh) {
        err = -ENOMEM;
        goto out;
    }

    sched_preempt_disable();

    exec_image_t old = exec_take_image(thread, fresh);

    uintptr_t entry_point = 0;
    arch_word_t entry_raw = 0;

    bool loaded = _load_user_segments(thread, &plan.file, &entry_raw);
    if (loaded) {
        entry_point = (uintptr_t)entry_raw;
    }

    if (!loaded) {
        log_warn("'%s' is not a valid executable for this arch", plan.file.resolved);
        exec_restore_image(thread, fresh, &old);
        sched_preempt_enable();
        err = -ENOEXEC;
        goto out;
    }

    uintptr_t stack_top = 0;
    if (!_map_user_stack(thread, &stack_top)) {
        exec_restore_image(thread, fresh, &old);
        sched_preempt_enable();
        err = -ENOMEM;
        goto out;
    }

    // the current thread's VM, stack, and trap frame switch as one image
    apply_identity(thread, plan.file.node);

    arch_vm_switch(thread->vm_space);
    stack_top = build_stack_args(stack_top, &plan.args, &plan.env);
    arch_code_sync();
    sched_signal_exec(thread);
    sched_fd_close_cloexec(thread);

    exec_drop_old_image(&old);

    if (state) {
        memset(state, 0, sizeof(*state));
        arch_set_user_entry(state, entry_point, stack_top);
        arch_state_set_return(state, 0);
        thread->context = (uintptr_t)state;
    } else {
        thread_prepare_user(thread, entry_point, stack_top);
    }

    const char *thread_name = plan.name;

    if (!plan.script && plan.args.argv[0]) {
        thread_name = plan.args.argv[0];
    } else if (!plan.script) {
        thread_name = path;
    }

    thread_set_name(thread, _basename(thread_name));

    sched_preempt_enable();

out:
    exec_plan_clear(&plan);

    return err;
}
