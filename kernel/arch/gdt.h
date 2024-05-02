#pragma once

#include <base/addr.h>
#include <base/attributes.h>
#include <base/types.h>

#define GDT_OFFSET(index) (u16)(index * sizeof(gdt_entry))

typedef struct PACKED {
    u16 limit_low;
    u16 base_low;
    u8 base_middle;
    u8 access;
    u8 flags;
    u8 base_high;
} gdt_entry;

typedef struct PACKED {
    u32 base_higher;
    u32 _reserved0;
} gdt_entry_high;

typedef struct PACKED {
    u32 _reserved0;
    u64 rsp[3];
    u64 _reserved1;
    u64 ist[7];
    u64 _reserved2;
    u16 _reserved3;
    u16 iopb;
} tss_entry;

typedef struct PACKED {
    u16 size;
    u64 gdt_ptr;
} gdt_desc;

#define GDT_ENTRY_COUNT 7

enum GDT_segments : u64 {
    GDT_kernel_code = GDT_OFFSET(1),
    GDT_kernel_data = GDT_OFFSET(2),
    GDT_user_code = GDT_OFFSET(3),
    GDT_user_data = GDT_OFFSET(4),
};

void gdt_init(void);
void tss_init(u64 kernel_stack_top);

void set_tss_stack(u64 stack);
