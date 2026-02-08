// #include <base/types.h>
// #include <stdint.h>
// #include <x86/asm.h>
//
// #include "gdt.h"
//
// static tss_entry_t tss = {0};
// static gdt_desc_t gdtd = {0};
//
// ALIGNED(0x10)
// static gdt_entry_t gdt_entries[GDT_ENTRY_COUNT] = {0};
//
//
// static void set_gdt_entry(size_t index, u64 base, u32 limit, u8 access, u8 flags) {
//     gdt_entries[index].limit_low = (limit & 0xffff);
//     gdt_entries[index].flags = (limit >> 16) & 0xf;
//
//     gdt_entries[index].base_low = (base & 0xffff);
//     gdt_entries[index].base_middle = (base >> 16) & 0xff;
//     gdt_entries[index].base_high = (base >> 24) & 0xff;
//
//     gdt_entries[index].access = access;
//
//     gdt_entries[index].flags |= (flags << 4);
// }
//
// static void set_gdt_high_entry(size_t index, u64 base) {
//     gdt_entry_high_t* high = (gdt_entry_high_t*)&gdt_entries[index + 1];
//     high->base_higher = (base >> 32) & 0xffffffff;
// }
//
// void gdt_init() {
//     gdtd.size = sizeof(gdt_entry_t) * GDT_ENTRY_COUNT - 1;
//     gdtd.gdt_ptr = (u64)gdt_entries;
//
//     set_gdt_entry(0, 0, 0, 0, 0); // Null segment
//     set_gdt_entry(1, 0, 0, 0x9a, 0x02); // Kernel code segment
//     set_gdt_entry(2, 0, 0, 0x92, 0x00); // Kernel data segment
//     set_gdt_entry(3, 0, 0, 0xfa, 0x02); // User code segment
//     set_gdt_entry(4, 0, 0, 0xf2, 0x00); // User data segment
//
//     asm volatile("lgdt %0" ::"m"(gdtd) : "memory");
// }
//
//
// void set_tss_stack(u64 stack) {
//     tss.rsp[0] = stack;
// }
//
// void tss_init() {
//     u64 tss_addr = (u64)(uintptr_t)&tss;
//
//     set_gdt_entry(5, tss_addr, sizeof(tss_entry_t) - 1, 0x89, 0);
//     set_gdt_high_entry(5, tss_addr);
//
//     asm volatile("ltr %0" ::"r"(GDT_OFFSET(5)) : "memory");
// }
