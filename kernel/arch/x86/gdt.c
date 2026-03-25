#include "gdt.h"

#include <base/attributes.h>
#include <base/types.h>
#include <log/log.h>
#include <stddef.h>
#include <string.h>
#include <sys/cpu.h>
#include <x86/apic.h>

static tss_entry_t tss_entries[MAX_CORES] = {0};
static gdt_desc_t gdt_descs[MAX_CORES] = {0};

#if defined(__x86_64__)
#define IST_STACK_SIZE 4096
ALIGNED(16)
static u8 ist_double_fault_stack[MAX_CORES][IST_STACK_SIZE];
#endif

ALIGNED(0x10)
static gdt_entry_t gdt_entries[MAX_CORES][GDT_ENTRY_COUNT] = {0};


bool gdt_current_core_id(size_t *out) {
    if (!out) {
        return false;
    }

    gdt_desc_t current = {0};
    asm volatile("sgdt %0" : "=m"(current));

    uintptr_t active_gdt = (uintptr_t)current.gdt_ptr;
    for (size_t i = 0; i < MAX_CORES; i++) {
        if (active_gdt == (uintptr_t)&gdt_entries[i][0]) {
            *out = i;
            return true;
        }
    }

    return false;
}

#if defined(__i386__)
static bool _lapic_core_id(size_t *out) {
    if (!out) {
        return false;
    }

    cpu_core_t *core = cpu_find_by_lapic(lapic_id());
    if (!core || !core->valid || core->id >= MAX_CORES) {
        return false;
    }

    *out = core->id;
    return true;
}
#endif

static size_t _gdt_core_id(void) {
    size_t core_id = 0;

#if defined(__i386__)
    if (_lapic_core_id(&core_id)) {
        return core_id;
    }
#endif

    if (gdt_current_core_id(&core_id)) {
        return core_id;
    }

    cpu_core_t *core = cpu_current();

    if (!core || core->id >= MAX_CORES) {
        return 0;
    }

    return core->id;
}

static gdt_entry_t *_gdt_entries_for(size_t core_id) {
    return gdt_entries[core_id];
}

static gdt_desc_t *_gdt_desc_for(size_t core_id) {
    return &gdt_descs[core_id];
}

static tss_entry_t *_tss_for(size_t core_id) {
    return &tss_entries[core_id];
}


static void
_set_gdt_entry(
    gdt_entry_t *entries,
    size_t index,
    u64 base,
    u32 limit,
    u8 access,
    u8 flags
) {
    entries[index].limit_low = (limit & 0xffff);
    entries[index].flags = (limit >> 16) & 0xf;
    entries[index].base_low = (base & 0xffff);
    entries[index].base_middle = (base >> 16) & 0xff;
    entries[index].base_high = (base >> 24) & 0xff;
    entries[index].access = access;
    entries[index].flags |= (flags << 4);
}

#if defined(__x86_64__)
static void _set_gdt_high_entry(gdt_entry_t *entries, size_t index, u64 base) {
    gdt_entry_high_t *high = (gdt_entry_high_t *)&entries[index + 1];
    high->base_higher = (base >> 32) & 0xffffffff;
}
#endif

static void _build_segments(size_t core_id) {
    gdt_entry_t *entries = _gdt_entries_for(core_id);

    memset(entries, 0, sizeof(gdt_entries[core_id]));

    u32 limit = 0;
    u8 code_flags = 0;
    u8 data_flags = 0;

#if defined(__x86_64__)
    limit = 0;
    code_flags = 0x02;
    data_flags = 0x00;
#else
    limit = 0xFFFFF;
    code_flags = 0x0C;
    data_flags = 0x0C;
#endif

    _set_gdt_entry(entries, 0, 0, 0, 0, 0); // Null segment
    _set_gdt_entry(entries, 1, 0, limit, 0x9a, code_flags); // Kernel code segment
    _set_gdt_entry(entries, 2, 0, limit, 0x92, data_flags); // Kernel data segment
    _set_gdt_entry(entries, 3, 0, limit, 0xfa, code_flags); // User code segment
    _set_gdt_entry(entries, 4, 0, limit, 0xf2, data_flags); // User data segment
}

void gdt_init(void) {
    size_t core_id = _gdt_core_id();
    gdt_desc_t *gdtd = _gdt_desc_for(core_id);

    log_debug("initializing GDT (core=%zu)", core_id);
    _build_segments(core_id);

    gdtd->size = sizeof(gdt_entry_t) * GDT_ENTRY_COUNT - 1;
#if defined(__x86_64__)
    gdtd->gdt_ptr = (u64)(uintptr_t)_gdt_entries_for(core_id);
#else
    gdtd->gdt_ptr = (u32)(uintptr_t)_gdt_entries_for(core_id);
#endif

    asm volatile("lgdt %0" : : "m"(*gdtd) : "memory");

#if defined(__i386__)
    asm volatile(
        "ljmp %0, $1f\n"
        "1:\n"
        "mov %1, %%ds\n"
        "mov %1, %%es\n"
        "mov %1, %%fs\n"
        "mov %1, %%gs\n"
        "mov %1, %%ss\n"
        :
        : "i"(GDT_KERNEL_CODE), "r"(GDT_KERNEL_DATA)
        : "memory"
    );
#else
    asm volatile(
        "pushq %0\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %1, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        :
        : "i"((u64)GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA)
        : "rax", "memory"
    );
#endif
}

void tss_init(uintptr_t kernel_stack_top) {
    size_t core_id = _gdt_core_id();
    tss_entry_t *tss = _tss_for(core_id);
    gdt_entry_t *entries = _gdt_entries_for(core_id);

    log_debug("initializing TSS (core=%zu)", core_id);

    memset(tss, 0, sizeof(*tss));

    u64 tss_addr = (u64)(uintptr_t)tss;

    _set_gdt_entry(entries, 5, tss_addr, sizeof(tss_entry_t) - 1, 0x89, 0);
#if defined(__x86_64__)
    _set_gdt_high_entry(entries, 5, tss_addr);
    tss->ist[0] = (u64)(uintptr_t)(ist_double_fault_stack[core_id] + IST_STACK_SIZE);
#endif

    set_tss_stack(kernel_stack_top);

    asm volatile("ltr %0" : : "r"(GDT_OFFSET(5)) : "memory");
}

void set_tss_stack(uintptr_t stack) {
    tss_entry_t *tss = _tss_for(_gdt_core_id());

#if defined(__x86_64__)
    tss->rsp[0] = (u64)stack;
#else
    tss->esp0 = (u32)stack;
    tss->ss0 = GDT_KERNEL_DATA;
#endif
}
