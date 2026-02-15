#include "gdt.h"

#include <base/attributes.h>
#include <base/types.h>
#include <log/log.h>
#include <stddef.h>

static tss_entry_t tss = {0};
static gdt_desc_t gdtd = {0};

ALIGNED(0x10)
static gdt_entry_t gdt_entries[GDT_ENTRY_COUNT] = {0};


static void _set_gdt_entry(size_t index, u64 base, u32 limit, u8 access, u8 flags) {
    gdt_entries[index].limit_low = (limit & 0xffff);
    gdt_entries[index].flags = (limit >> 16) & 0xf;

    gdt_entries[index].base_low = (base & 0xffff);
    gdt_entries[index].base_middle = (base >> 16) & 0xff;
    gdt_entries[index].base_high = (base >> 24) & 0xff;

    gdt_entries[index].access = access;

    gdt_entries[index].flags |= (flags << 4);
}

#if defined(__x86_64__)
static void _set_gdt_high_entry(size_t index, u64 base) {
    gdt_entry_high_t* high = (gdt_entry_high_t*)&gdt_entries[index + 1];
    high->base_higher = (base >> 32) & 0xffffffff;
}
#endif

void gdt_init(void) {
    log_debug("initializing GDT");
    gdtd.size = sizeof(gdt_entry_t) * GDT_ENTRY_COUNT - 1;
#if defined(__x86_64__)
    gdtd.gdt_ptr = (u64)(uintptr_t)gdt_entries;
#else
    gdtd.gdt_ptr = (u32)(uintptr_t)gdt_entries;
#endif

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

    _set_gdt_entry(0, 0, 0, 0, 0); // Null segment
    _set_gdt_entry(1, 0, limit, 0x9a, code_flags); // Kernel code segment
    _set_gdt_entry(2, 0, limit, 0x92, data_flags); // Kernel data segment
    _set_gdt_entry(3, 0, limit, 0xfa, code_flags); // User code segment
    _set_gdt_entry(4, 0, limit, 0xf2, data_flags); // User data segment

    asm volatile("lgdt %0" : : "m"(gdtd) : "memory");

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
    log_debug("initializing TSS");
    u64 tss_addr = (u64)(uintptr_t)&tss;

    _set_gdt_entry(5, tss_addr, sizeof(tss_entry_t) - 1, 0x89, 0);
#if defined(__x86_64__)
    _set_gdt_high_entry(5, tss_addr);
#endif

    set_tss_stack(kernel_stack_top);

    asm volatile("ltr %0" : : "r"(GDT_OFFSET(5)) : "memory");
}

void set_tss_stack(uintptr_t stack) {
#if defined(__x86_64__)
    tss.rsp[0] = (u64)stack;
#else
    tss.esp0 = (u32)stack;
    tss.ss0 = GDT_KERNEL_DATA;
#endif
}
