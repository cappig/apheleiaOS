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
    u8 accessed               : 1;
    u8 read_write             : 1;
    u8 conforming_expand_down : 1;
    u8 code                   : 1;
    u8 code_data_segment      : 1;
    u8 privilege              : 2;
    u8 present                : 1;
} gdt_access_byte;

typedef struct PACKED {
    u8 limit_high  : 4;
    u8 free_bit    : 1;
    u8 long_mode   : 1;
    u8 big         : 1;
    u8 granularity : 1;
} gdt_flags_byte;

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

enum gdt_segments {
    GDT_kernel_code = GDT_OFFSET(1),
    GDT_kernel_data = GDT_OFFSET(2),
    GDT_user_code = GDT_OFFSET(3),
    GDT_user_data = GDT_OFFSET(4),
};


void gdt_init(void);
void tss_init(u64 kernel_stack_top);

void set_tss_stack(u64 stack);
