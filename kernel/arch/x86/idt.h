#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <x86/regs.h>

#define IRQ_OFFSET 32
#define ISR_COUNT  256

#define IRQ_INT(irq) ((irq) + IRQ_OFFSET)

#define EXCEPTION_COUNT 32

#define IDT_INT 0x8E
#define IDT_TRP 0xEF

#define INT_SPURIOUS 0xFF

#if defined(__x86_64__)
typedef struct PACKED {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 attributes;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved0;
} idt_entry_t;

typedef struct PACKED {
    u16 limit;
    u64 base;
} idt_register_t;
#else
typedef struct PACKED {
    u16 offset_low;
    u16 selector;
    u8 zero;
    u8 attributes;
    u16 offset_high;
} idt_entry_t;

typedef struct PACKED {
    u16 limit;
    u32 base;
} idt_register_t;
#endif

// https://wiki.osdev.org/Exceptions
// https://wiki.osdev.org/Interrupts
enum exception_numbers {
    INT_DIVIDE_BY_ZERO = 0x00,
    INT_SINGLE_STEP = 0x01,
    INT_NON_MASKABLE = 0x02,
    INT_BREAKPOINT = 0x03,
    INT_OVERFLOW = 0x04,
    INT_OUT_OF_BOUNDS = 0x05,
    INT_INVALID_OPCODE = 0x06,
    INT_NO_COPROCESSOR = 0x07,
    INT_DOUBLE_FAULT = 0x08,
    INT_COPROCESSOR_SEGOVER = 0x09,
    INT_INVALID_TSS = 0x0A,
    INT_SEGMENT_NOT_PRESENT = 0x0B,
    INT_STACK_SEGMENT_FAULT = 0x0C,
    INT_GENERAL_PROTECTION_FAULT = 0x0D,
    INT_PAGE_FAULT = 0x0E,
    INT_FLOATING_POINT_EXCEPTION = 0x10,
    INT_ALIGNMENT_CHECK = 0x11,
    INT_MACHINE_CHECK = 0x12,
    INT_SIMD_FLOATING_POINT_EXCEPTION = 0x13,
    INT_VIRTUALIZATION_EXCEPTION = 0x14,
    INT_CONTROL_PROTECTION_EXCEPTION = 0x15,
    INT_HYPERVISOR_INJECTION_EXCEPTION = 0x1C,
    INT_VMM_COMMUNICATION_EXCEPTION = 0x1D,
    INT_SECURITY_EXCEPTION = 0x1E,
};

typedef struct PACKED {
    gen_regs_t g_regs;

    // Pushed by the isr_stub_xx
#if defined(__x86_64__)
    u64 int_num;
    u64 error_code;
#else
    u32 int_num;
    u32 error_code;
#endif

    spec_regs_t s_regs;
} int_state_t;

typedef void (*int_handler_t)(int_state_t *state);


void set_int_handler(size_t int_num, int_handler_t handler);
void reset_int_handler(size_t int_num);
void configure_int(size_t int_num, u16 selector, u8 ist, u8 attribs);

void idt_init(void);
