#include "exec.h"

#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <ctype.h>
#include <errno.h>
#include <log/log.h>
#include <parse/elf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>
#include <unistd.h>

#define USER_STACK_PAGES 16

static uintptr_t next_stack_top;

typedef struct {
    int argc;
    char* argv[EXEC_MAX_ARGS + 1];
} exec_args_t;

typedef struct {
    char path[PATH_MAX];
    char arg[EXEC_MAX_ARG_LEN];
    bool has_arg;
} exec_shebang_t;

typedef struct {
    char resolved[PATH_MAX];
    vfs_node_t* node;
    u8* buffer;
    size_t size;
} exec_file_t;

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

static size_t _strnlen(const char* str, size_t max) {
    if (!str)
        return 0;

    size_t len = 0;
    while (len < max && str[len])
        len++;

    return len;
}

static bool _args_push(exec_args_t* args, const char* value) {
    if (!args || !value)
        return false;

    if (args->argc >= EXEC_MAX_ARGS)
        return false;

    size_t len = _strnlen(value, EXEC_MAX_ARG_LEN);
    if (len >= EXEC_MAX_ARG_LEN)
        len = EXEC_MAX_ARG_LEN - 1;

    char* copy = malloc(len + 1);
    if (!copy)
        return false;

    memcpy(copy, value, len);
    copy[len] = '\0';

    args->argv[args->argc++] = copy;
    args->argv[args->argc] = NULL;
    return true;
}

static void _free_args(exec_args_t* args) {
    if (!args)
        return;

    for (int i = 0; i < args->argc; i++) {
        free(args->argv[i]);
        args->argv[i] = NULL;
    }

    args->argc = 0;
    args->argv[0] = NULL;
}

static bool _copy_args(char* const argv[], exec_args_t* out) {
    if (!out)
        return false;

    memset(out, 0, sizeof(*out));

    if (!argv)
        return true;

    for (int i = 0; i < EXEC_MAX_ARGS; i++) {
        const char* arg = argv[i];
        if (!arg)
            break;

        if (!_args_push(out, arg)) {
            _free_args(out);
            return false;
        }
    }

    out->argv[out->argc] = NULL;
    return true;
}

static uintptr_t _build_user_stack_args(uintptr_t stack_top, const exec_args_t* args) {
    uintptr_t sp = stack_top;
    size_t argc = args ? (size_t)args->argc : 0;
    uintptr_t arg_ptrs[EXEC_MAX_ARGS] = {0};

    for (int i = (int)argc - 1; i >= 0; i--) {
        size_t len = strlen(args->argv[i]) + 1;
        sp -= len;
        memcpy((void*)sp, args->argv[i], len);
        arg_ptrs[i] = sp;
    }

    sp = ALIGN_DOWN(sp, 16);

    size_t slots = argc + 3;
    if ((slots % 2) != 0) {
        sp -= sizeof(uintptr_t);
        *(uintptr_t*)sp = 0;
    }

    sp -= sizeof(uintptr_t);
    *(uintptr_t*)sp = 0;

    sp -= sizeof(uintptr_t);
    *(uintptr_t*)sp = 0;

    for (int i = (int)argc - 1; i >= 0; i--) {
        sp -= sizeof(uintptr_t);
        *(uintptr_t*)sp = arg_ptrs[i];
    }

    sp -= sizeof(uintptr_t);
    *(uintptr_t*)sp = (uintptr_t)argc;

    return sp;
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

static void _close_file(exec_file_t* file) {
    if (!file)
        return;

    free(file->buffer);
    file->buffer = NULL;
    file->size = 0;
    file->node = NULL;
    file->resolved[0] = '\0';
}

static int
_open_file(sched_thread_t* thread, const char* path, exec_file_t* out, bool require_exec) {
    if (!thread || !path || !out)
        return -EINVAL;

    memset(out, 0, sizeof(*out));

    if (!path_resolve(thread->cwd, path, out->resolved, sizeof(out->resolved)))
        return -ENOENT;

    out->node = vfs_lookup(out->resolved);
    if (!out->node) {
        log_warn("exec: '%s' not found", out->resolved);
        return -ENOENT;
    }

    if (out->node->type != VFS_FILE) {
        log_warn("exec: '%s' is not a file (type=%u)", out->resolved, out->node->type);
        return -EISDIR;
    }

    if (require_exec && !vfs_access(out->node, thread->uid, thread->gid, X_OK)) {
        log_warn("exec: '%s' is not executable", out->resolved);
        return -EACCES;
    }

    if (!read_file(out->node, &out->buffer, &out->size)) {
        log_warn("exec: failed to read '%s'", out->resolved);
        _close_file(out);
        return -EIO;
    }

    return 0;
}

static bool _parse_shebang(const u8* buffer, size_t size, exec_shebang_t* out) {
    if (!buffer || size < 2 || !out)
        return false;

    if (buffer[0] != '#' || buffer[1] != '!')
        return false;

    size_t idx = 2;
    while (idx < size && (buffer[idx] == ' ' || buffer[idx] == '\t'))
        idx++;

    size_t start = idx;
    while (idx < size) {
        u8 ch = buffer[idx];
        if (ch == '\n' || ch == '\r' || isspace((int)ch))
            break;
        idx++;
    }

    size_t len = idx - start;
    if (!len)
        return false;

    memset(out, 0, sizeof(*out));
    if (len >= sizeof(out->path))
        len = sizeof(out->path) - 1;
    memcpy(out->path, buffer + start, len);
    out->path[len] = '\0';

    while (idx < size && (buffer[idx] == ' ' || buffer[idx] == '\t'))
        idx++;

    if (idx < size && buffer[idx] != '\n' && buffer[idx] != '\r') {
        start = idx;
        while (idx < size) {
            u8 ch = buffer[idx];
            if (ch == '\n' || ch == '\r' || isspace((int)ch))
                break;
            idx++;
        }

        len = idx - start;
        if (len) {
            if (len >= sizeof(out->arg))
                len = sizeof(out->arg) - 1;
            memcpy(out->arg, buffer + start, len);
            out->arg[len] = '\0';
            out->has_arg = true;
        }
    }

    return true;
}

static bool _build_shebang_args(
    const exec_args_t* orig,
    const exec_shebang_t* shebang,
    const char* script_path,
    exec_args_t* out
) {
    if (!shebang || !script_path || !out)
        return false;

    memset(out, 0, sizeof(*out));

    if (!_args_push(out, shebang->path)) {
        _free_args(out);
        return false;
    }

    if (shebang->has_arg) {
        if (!_args_push(out, shebang->arg)) {
            _free_args(out);
            return false;
        }
    }

    if (!_args_push(out, script_path)) {
        _free_args(out);
        return false;
    }

    if (orig) {
        for (int i = 1; i < orig->argc; i++) {
            if (!_args_push(out, orig->argv[i])) {
                _free_args(out);
                return false;
            }
        }
    }

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

    exec_file_t file = {0};
    int err = _open_file(thread, path, &file, true);
    if (err) {
        sched_discard_thread(thread);
        return NULL;
    }

    char exec_name_buf[PATH_MAX];
    strncpy(exec_name_buf, file.resolved, sizeof(exec_name_buf) - 1);
    exec_name_buf[sizeof(exec_name_buf) - 1] = '\0';

    exec_shebang_t shebang = {0};
    exec_args_t args = {0};
    bool is_script = _parse_shebang(file.buffer, file.size, &shebang);

    if (is_script) {
        if (!_build_shebang_args(NULL, &shebang, exec_name_buf, &args)) {
            _close_file(&file);
            sched_discard_thread(thread);
            return NULL;
        }

        exec_file_t interp = {0};
        err = _open_file(thread, shebang.path, &interp, true);
        if (err) {
            _free_args(&args);
            _close_file(&file);
            sched_discard_thread(thread);
            return NULL;
        }

        _close_file(&file);
        file = interp;
    } else {
        if (!_args_push(&args, path)) {
            _close_file(&file);
            sched_discard_thread(thread);
            return NULL;
        }
    }

    uintptr_t entry = 0;
    arch_word_t entry_raw = 0;
    bool ok = load_user_segments(thread, file.buffer, file.size, &entry_raw);
    if (ok)
        entry = (uintptr_t)entry_raw;

    if (!ok) {
        log_warn("exec: '%s' is not a valid executable for this arch", file.resolved);
        _free_args(&args);
        _close_file(&file);
        sched_discard_thread(thread);
        return NULL;
    }

    uintptr_t stack_top = 0;
    if (!_map_user_stack(thread, &stack_top)) {
        log_warn("exec: failed to map user stack");
        _free_args(&args);
        _close_file(&file);
        sched_discard_thread(thread);
        return NULL;
    }

    arch_vm_switch(thread->vm_space);
    stack_top = _build_user_stack_args(stack_top, &args);
    arch_vm_switch(arch_vm_kernel());

    sched_prepare_user_thread(thread, entry, stack_top);
    thread->ppid = 0;
    sched_set_thread_name(thread, exec_basename(exec_name_buf));

    thread->state = THREAD_READY;
    _free_args(&args);
    _close_file(&file);
    return thread;
}

int user_exec(sched_thread_t* thread, const char* path, arch_int_state_t* state) {
    if (!thread || !path)
        return -EINVAL;

    (void)envp;

    exec_args_t args = {0};
    if (!_copy_args(argv, &args))
        return -ENOMEM;

    exec_file_t file = {0};
    int err = _open_file(thread, path, &file, true);
    if (err) {
        _free_args(&args);
        return err;
    }

    char exec_name_buf[PATH_MAX];
    strncpy(exec_name_buf, file.resolved, sizeof(exec_name_buf) - 1);
    exec_name_buf[sizeof(exec_name_buf) - 1] = '\0';

    exec_shebang_t shebang = {0};
    exec_args_t script_args = {0};
    bool is_script = _parse_shebang(file.buffer, file.size, &shebang);

    if (is_script) {
        if (!_build_shebang_args(&args, &shebang, exec_name_buf, &script_args)) {
            _free_args(&args);
            _close_file(&file);
            return -ENOMEM;
        }

        _free_args(&args);
        args = script_args;

        exec_file_t interp = {0};
        err = _open_file(thread, shebang.path, &interp, true);
        if (err) {
            _free_args(&args);
            _close_file(&file);
            return err;
        }

        _close_file(&file);
        file = interp;
    }

    arch_vm_space_t* old_vm = thread->vm_space;
    arch_vm_space_t* fresh = arch_vm_create_user();
    if (!fresh) {
        _free_args(&args);
        _close_file(&file);
        return -ENOMEM;
    }

    sched_user_region_t* old_regions = thread->regions;
    uintptr_t old_stack_base = thread->user_stack_base;
    size_t old_stack_size = thread->user_stack_size;

    thread->vm_space = fresh;
    thread->regions = NULL;
    thread->user_stack_base = old_stack_base;
    thread->user_stack_size = old_stack_size;

    uintptr_t entry_point = 0;
    arch_word_t entry_raw = 0;
    bool ok = load_user_segments(thread, file.buffer, file.size, &entry_raw);
    if (ok)
        entry_point = (uintptr_t)entry_raw;

    if (!ok) {
        log_warn("exec: '%s' is not a valid executable for this arch", file.resolved);
        sched_clear_user_regions(thread);
        thread->regions = old_regions;
        thread->vm_space = old_vm;
        arch_vm_destroy(fresh);
        _free_args(&args);
        _close_file(&file);
        return -ENOEXEC;
    }

    uintptr_t stack_top = 0;
    if (!_map_user_stack(thread, &stack_top)) {
        _free_regions_list(thread->regions);
        thread->regions = old_regions;
        thread->vm_space = old_vm;
        arch_vm_destroy(fresh);
        _free_args(&args);
        _close_file(&file);
        return -ENOMEM;
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

    const char* thread_name = exec_name_buf;
    if (!is_script && args.argv[0])
        thread_name = args.argv[0];
    else if (!is_script)
        thread_name = path;

    sched_set_thread_name(thread, exec_basename(thread_name));

    _free_args(&args);
    _close_file(&file);
    return 0;
}
