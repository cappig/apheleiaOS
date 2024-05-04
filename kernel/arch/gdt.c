#include "gdt.h"

#include <base/types.h>
#include <x86/asm.h>

static tss_entry tss;
static gdt_desc gdtp;

ALIGNED(0x10)
static gdt_entry gdt_entries[GDT_ENTRY_COUNT];


// This should probably be computed at compile time
static void set_gdt_entry(usize index, u64 base, u32 limit, u8 access, u8 flags) {
    gdt_entries[index].limit_low = (limit & 0xffff);
    gdt_entries[index].flags = (limit >> 16) & 0x0f;

    gdt_entries[index].base_low = (base & 0xffff);
    gdt_entries[index].base_middle = (base >> 16) & 0xff;
    gdt_entries[index].base_high = (base >> 24) & 0xff;

    gdt_entries[index].access = access;

    gdt_entries[index].flags |= (flags << 4);
}

static void set_gdt_high_entry(usize index, u64 base) {
    gdt_entries[index + 1].base_high = (base >> 32) & 0xffffffff;
}

void gdt_init() {
    gdtp.size = sizeof(gdt_entry) * GDT_ENTRY_COUNT - 1;
    gdtp.gdt_ptr = (u64)gdt_entries;

    set_gdt_entry(0, 0, 0, 0, 0); // Null segment
    set_gdt_entry(1, 0, 0xfffff, 0x9a, 0x0a); // Code segment
    set_gdt_entry(2, 0, 0xfffff, 0x92, 0x0a); // Data segment (offset 0x10)
    set_gdt_entry(3, 0, 0xfffff, 0xfa, 0x0a); // User mode code segment
    set_gdt_entry(4, 0, 0xfffff, 0xf2, 0x0a); // User mode data segment

    asm volatile("lgdt %0" ::"m"(gdtp) : "memory");
}

void tss_init(u64 kernel_stack_top) {
    u64 tss_addr = (u64)(uptr)&tss;

    set_gdt_entry(5, tss_addr, sizeof(tss_entry), 0x89, 0x00);
    set_gdt_high_entry(5, tss_addr);

    set_tss_stack(kernel_stack_top);

    asm volatile("ltr %0" ::"r"(GDT_OFFSET(5)) : "memory");
}

void set_tss_stack(u64 stack) {
    tss.rsp[0] = stack;
}
