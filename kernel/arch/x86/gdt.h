#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define GDT_OFFSET(index) ((u16)((index) * sizeof(gdt_entry_t)))

#define GDT_ENTRY_COUNT 7

typedef struct PACKED {
    u16 limit_low;
    u16 base_low;
    u8 base_middle;
    u8 access;
    u8 flags;
    u8 base_high;
} gdt_entry_t;

#if defined(__x86_64__)
typedef struct PACKED {
    u32 base_higher;
    u32 _reserved0;
} gdt_entry_high_t;
#endif

typedef struct PACKED {
    u8 accessed               : 1;
    u8 read_write             : 1;
    u8 conforming_expand_down : 1;
    u8 code                   : 1;
    u8 code_data_segment      : 1;
    u8 privilege              : 2;
    u8 present                : 1;
} gdt_access_byte_t;

typedef struct PACKED {
    u8 limit_high  : 4;
    u8 free_bit    : 1;
    u8 long_mode   : 1;
    u8 big         : 1;
    u8 granularity : 1;
} gdt_flags_byte_t;

#if defined(__x86_64__)
typedef struct PACKED {
    u32 _reserved0;
    u64 rsp[3];
    u64 _reserved1;
    u64 ist[7];
    u64 _reserved2;
    u16 _reserved3;
    u16 iopb;
} tss_entry_t;
#else
typedef struct PACKED {
    u16 prev_tss;
    u16 _reserved0;
    u32 esp0;
    u16 ss0;
    u16 _reserved1;
    u32 esp1;
    u16 ss1;
    u16 _reserved2;
    u32 esp2;
    u16 ss2;
    u16 _reserved3;
    u32 cr3;
    u32 eip;
    u32 eflags;
    u32 eax, ecx, edx, ebx;
    u32 esp, ebp, esi, edi;
    u16 es, _res1;
    u16 cs, _res2;
    u16 ss, _res3;
    u16 ds, _res4;
    u16 fs, _res5;
    u16 gs, _res6;
    u16 ldt_selector, _res7;
    u16 trap;
    u16 iopb;
} tss_entry_t;
#endif

typedef struct PACKED {
    u16 size;
#if defined(__x86_64__)
    u64 gdt_ptr;
#else
    u32 gdt_ptr;
#endif
} gdt_desc_t;

enum gdt_segments {
    GDT_KERNEL_CODE = GDT_OFFSET(1),
    GDT_KERNEL_DATA = GDT_OFFSET(2),
    GDT_USER_CODE = GDT_OFFSET(3),
    GDT_USER_DATA = GDT_OFFSET(4),
};


void gdt_init(void);
void tss_init(uintptr_t kernel_stack_top);
void set_tss_stack(uintptr_t stack);
bool gdt_current_core_id(size_t *out);
