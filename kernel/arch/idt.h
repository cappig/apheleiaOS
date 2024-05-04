#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <x86/regs.h>

#define IRQ_OFFSET 32

#define ISR_COUNT 256

#define IRQ_COUNT 16
#define INT_COUNT 32

#define IRQ_NUMBER(irq) (irq + INT_COUNT)

#define IS_IRQ(irq) (irq >= INT_COUNT && irq < IRQ_COUNT + INT_COUNT)

typedef struct PACKED {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 attributes;
    u16 offset_mid;
    u32 offset_high;
    u32 _unsued0;
} idt_entry;

typedef struct PACKED {
    u16 limit;
    u64 base;
} idt_register;

// https://wiki.osdev.org/Exceptions
// https://wiki.osdev.org/Interrupts
typedef enum {
    INT_DIVIDE_BY_ZERO = 0x00,
    INT_SINGLE_STEP = 0x01,
    INT_NON_MASKABLE = 0x02,
    INT_BREAKPOINT = 0x03,
    INT_OVERFLOW = 0x04,
    INT_OUT_OF_BOUNDS = 0x05,
    INT_INVALID_OPCODE = 0x06,
    INT_NO_COPROCESSOR = 0x07,
    INT_DOUBLE_FAULT = 0x08,
    INT_COPROCESSOR_SEGOVER = 0x09, // not used after i386, ignored
    INT_INVALID_TSS = 0x0a,
    INT_SEGMENT_NOT_PRESENT = 0x0b,
    INT_STACK_SEGMENT_FAULT = 0x0c,
    INT_GENERAL_PROTECTION_FAULT = 0x0d,
    INT_PAGE_FAULT = 0x0e,
    // 0x0f = reserved
    INT_X87_FLOATING_POINT_EXCEPTION = 0x10,
    INT_ALIGNMENT_CHECK = 0x11,
    INT_MACHINE_CHECK = 0x12,
    INT_SIMD_FLOATING_POINT_EXCEPTION = 0x13,
    INT_VIRTUALIZATION_EXCEPTION = 0x14,
    INT_CONTROL_PROTECTION_EXCEPTION = 0x15,
    // 0x16 - 0x1b = reserved
    INT_HYPERVISOR_INJECTION_EXCEPTION = 0x1c,
    INT_VMM_COMMUNICATION_EXCEPTION = 0x1d,
    INT_SECURITY_EXCEPTION = 0x1e,
} int_numbers;

typedef enum {
    IRQ_SYSTEM_TIMER = 0x00,
    IRQ_PS2_KEYBOARD = 0x01,
    IRQ_CASCADE = 0x02,
    IRQ_COM2 = 0x03,
    IRQ_COM1 = 0x04,
    IRQ_LPT2 = 0x05,
    IRQ_FLOPPY = 0x06,
    IRQ_SPURIOUS = 0x07, // ignored
    IRQ_CMOS_RTC = 0x08,
    IRQ_OPEN_9 = 0x09,
    IRQ_OPEN_10 = 0x0a,
    IRQ_OPEN_11 = 0x0b,
    IRQ_PS2_MOUSE = 0x0c,
    IRQ_COPROCESSOR = 0x0d,
    IRQ_PRIMARY_ATA = 0x0e,
    IRQ_SECONDARY_ATA = 0x0f,
} irq_numbers;

// "?" indicate a reserved int number
static const char* int_strings[32] = {
    "Divide by zero",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "Out of bounds",
    "Invalid opcode",
    "No coprocessor",
    "Double fault",
    "Coprocessor segover",
    "Invalid tss",
    "Segment not present",
    "Stack segment fault",
    "General protection fault",
    "Page fault",
    "?",
    "x87 floating point exception",
    "Alignment check",
    "Machine check",
    "SIMD floating point exception",
    "Virtualization exception",
    "Control protection exception",
    "?",
    "?",
    "?",
    "?",
    "?",
    "Hypervisor injection exception",
    "Vmm communication exception",
    "Security exception",
    "?",
};

// TODO: move this
/*
#define PF_STRINGS_COUNT 7

typedef enum {
    PF_PROTECTION_VIOLATION = 1,
    PF_WRITE = 1 << 1,
    PF_IN_USER_MODE = 1 << 2,
    PF_RESERVED_WRITE = 1 << 3,
    PF_INSTRUCTION_FETCH = 1 << 4,
    PF_PROTECTION_KEY = 1 << 5,
    PF_SHADOW_STACK = 1 << 5,
} page_fault_err_codes;

static kv_pair pf_strings[PF_STRINGS_COUNT] = {
    { PF_PROTECTION_VIOLATION, "Page protection violation" },
    { PF_WRITE, "Caused by write" },
    { PF_IN_USER_MODE, "Occurred while in user mode" },
    { PF_RESERVED_WRITE, "Write to reserved page" },
    { PF_INSTRUCTION_FETCH, "Caused by instruction fetch" },
    { PF_PROTECTION_KEY, "Protection key violation" },
    { PF_SHADOW_STACK, "Caused by shadow stack access" }
};
*/

// Save the machine state in the order that they are pushed to the stack
// Values at the top are pushed last
typedef struct PACKED {
    // Save all the general registers so that we can dump them if a fatal
    // exception occurs
    gen_regs g_regs;

    // Pushed by the isr_stub_xx
    u64 int_num;
    u64 error_code;

    // The INT instruction pushes these regs to the stack
    // https://wiki.osdev.org/Interrupt_Service_Routines
    spec_regs s_regs;
} int_state;

// All interrupt handlers must be of this type
typedef void (*int_handler)(int_state*);

void idt_init(void);

void set_int_handler(usize int_num, int_handler handler);
