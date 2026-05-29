#include "memory.h"

#include <alloc/bitmap.h>
#include <base/macros.h>
#include <data/bitmap.h>
#include <log/log.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bios.h"
#include "stdlib.h"
#include "tty.h"
#include "x86/e820.h"

#define SMAP 0x534d4150

static e820_map_t *e820_mmap = NULL;

void get_e820(e820_map_t *mmap) {
    regs32_t out_regs = { 0 };
    regs32_t in_regs = {
        .eax = 0xe820,
        .ecx = sizeof(e820_entry_t),
        .edx = SMAP,
    };

    while (mmap->count < E820_MAX) {
        e820_entry_t entry = { 0 };

        in_regs.edi = REAL_OFF(&entry);
        in_regs.es = REAL_SEG(&entry);

        bios_call(0x15, &in_regs, &out_regs);

        in_regs.ebx = out_regs.ebx;

        if (out_regs.eflags & FLAG_CF || out_regs.eax != SMAP) {
            break;
        }

        if (out_regs.ecx < 20 || out_regs.ecx > sizeof(e820_entry_t)) {
            break;
        }

        mmap->entries[mmap->count] = entry;
        mmap->count++;

        if (!out_regs.ebx) {
            break;
        }
    }

    e820_mmap = mmap;

    log_debug("e820 memory regions=%llu", (unsigned long long)mmap->count);
}

void get_rsdp(u64 *rsdp) {
    u16 ebda_seg = 0;
    asm volatile("movw 0x40e, %0" : "=r"(ebda_seg));
    u64 ebda = ((u64)ebda_seg) << 4;

    for (u64 addr = ebda; addr <= 0xfffff; addr += 16) {
        if (addr == ebda + 1024) {
            addr = 0xe0000;
        }

        if (!strncmp((char *)(uintptr_t)addr, "RSD PTR ", 8)) {
            *rsdp = addr;
            log_debug("RSDP found at %#llx", addr);

            return;
        }
    }

    log_debug("RSDP not found");
}

void *mmap_alloc(size_t size, int type, size_t alignment) {
    if (!size) {
        panic("attempted to allocate zero bytes");
    }

    void *ret = mmap_alloc_inner(e820_mmap, size, type, alignment, (size_t)-1);

    if (!ret) {
        panic("out of memory");
    }

    return ret;
}

void *mmap_alloc_top(size_t size, int type, size_t alignment, u64 top) {
    if (!size) {
        panic("attempted to allocate zero bytes");
    }

    void *ret = mmap_alloc_inner(e820_mmap, size, type, alignment, top);

    if (!ret) {
        panic("out of memory");
    }

    return ret;
}

void *mmap_try_alloc_top(size_t size, int type, size_t alignment, u64 top) {
    if (!size) {
        return NULL;
    }

    return mmap_alloc_inner(e820_mmap, size, type, alignment, top);
}

static void *balloc(size_t size) {
    return mmap_alloc(size, E820_ALLOC, 1);
}

static void bfree(void *ptr) {
    if (mmap_free_inner(e820_mmap, ptr)) {
        panic("attempted to free non-allocated memory");
    }
}

void arch_init_alloc() {
    libc_alloc_ops_t ops = {
        .malloc_fn = balloc,
        .free_fn = bfree,
    };
    __libc_init_alloc(&ops);
}
