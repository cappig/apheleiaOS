#include "usercopy.h"

#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <base/macros.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

static bool _user_page_flags_ok(
    const sched_thread_t *thread,
    uintptr_t addr,
    bool write
) {
    if (!thread || !thread->vm_space) {
        return false;
    }

    void *root = arch_vm_root(thread->vm_space);
    if (!root) {
        return false;
    }

    page_t *entry = NULL;
    size_t size = arch_get_page(root, addr, &entry);
    if (!entry || !size || !(*entry & PT_PRESENT) || !(*entry & PT_USER)) {
        return false;
    }

    if (write && !(*entry & PT_WRITE)) {
        return false;
    }

    return true;
}

bool user_range_ok(
    const sched_thread_t *thread,
    const void *ptr,
    size_t len,
    bool write
) {
    if (!len) {
        return true;
    }

    if (!thread || !thread->user_thread || !ptr) {
        return false;
    }

    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + len;
    uintptr_t user_top = (uintptr_t)arch_user_stack_top();

    if (end <= start || !user_top || end > user_top) {
        return false;
    }

    uintptr_t cursor = start;
    while (cursor < end) {
        const sched_user_region_t *match = NULL;
        uintptr_t match_end = 0;

        // The region list says what should exist; the PTEs say what exists now.
        for (
            const sched_user_region_t *region = thread->regions;
            region;
            region = region->next
        ) {
            uintptr_t region_start = region->vaddr;
            if (region->pages > SIZE_MAX / PAGE_4KIB) {
                return false;
            }

            uintptr_t region_size = region->pages * PAGE_4KIB;
            uintptr_t region_end = region_start + region_size;

            if (region_end <= region_start) {
                return false;
            }

            if (cursor >= region_start && cursor < region_end) {
                match = region;
                match_end = region_end;
                break;
            }
        }

        if (!match || !(match->flags & PT_USER)) {
            return false;
        }

        if (write && !(match->flags & PT_WRITE)) {
            return false;
        }

        uintptr_t segment_end = match_end < end ? match_end : end;
        bool require_write = write && !(match->flags & SCHED_REGION_COW);

        // COW pages are fine for reads. Writes have to fault them private first.
        for (
            uintptr_t page = ALIGN_DOWN(cursor, PAGE_4KIB);
            page < segment_end;
            page += PAGE_4KIB
        ) {
            if (!_user_page_flags_ok(thread, page, require_write)) {
                return false;
            }
        }

        cursor = segment_end;
    }

    return true;
}

bool user_write_prepare(const sched_thread_t *thread, void *ptr, size_t len) {
    if (!user_range_ok(thread, ptr, len, true)) {
        return false;
    }

    if (!len) {
        return true;
    }

    uintptr_t start = ALIGN_DOWN((uintptr_t)ptr, PAGE_4KIB);
    uintptr_t end = (uintptr_t)ptr + len;

    for (uintptr_t page = start; page < end; page += PAGE_4KIB) {
        if (_user_page_flags_ok(thread, page, true)) {
            continue;
        }

        if (!sched_handle_cow_fault((sched_thread_t *)thread, page, true)) {
            return false;
        }

        if (!_user_page_flags_ok(thread, page, true)) {
            return false;
        }
    }

    return true;
}

bool user_copy_from(
    const sched_thread_t *thread,
    void *dst,
    const void *src,
    size_t len
) {
    if (!len) {
        return true;
    }

    if (!dst || !user_range_ok(thread, src, len, false)) {
        return false;
    }

    memcpy(dst, src, len);
    return true;
}

bool user_copy_to(
    const sched_thread_t *thread,
    void *dst,
    const void *src,
    size_t len
) {
    if (!len) {
        return true;
    }

    if (!src || !user_write_prepare(thread, dst, len)) {
        return false;
    }

    memcpy(dst, src, len);
    return true;
}

int user_copy_string(
    const sched_thread_t *thread,
    const char *src,
    char *dst,
    size_t dst_len
) {
    if (!src || !dst || !dst_len) {
        return -EFAULT;
    }

    for (size_t i = 0; i < dst_len; i++) {
        const char *user_ch = (const char *)((uintptr_t)src + i);
        if (!user_range_ok(thread, user_ch, 1, false)) {
            return -EFAULT;
        }

        dst[i] = *user_ch;
        if (!dst[i]) {
            return 0;
        }
    }

    dst[dst_len - 1] = '\0';
    return -ENAMETOOLONG;
}
