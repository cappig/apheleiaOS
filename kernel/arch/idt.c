#include "idt.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <log/log.h>
#include <x86/asm.h>
#include <x86/paging.h>
#include <x86/regs.h>

#include "arch/gdt.h"
#include "arch/stacktrace.h"
#include "sched/process.h"
#include "sched/scheduler.h"
#include "sched/signal.h"
#include "sys/cpu.h"
#include "sys/panic.h"

static idt_register idtr;

extern void* isr_stub_table[ISR_COUNT];
static int_handler int_handlers[ISR_COUNT];

ALIGNED(0x10)
static idt_entry idt_entries[ISR_COUNT] = {0};

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
    "?",
};

static void generic_int_handler(int_state* s) {
    log_warn("Unhandled interrupt: [int=%#lx]\n", s->int_num);
}

static isize _exception_to_signal(usize int_num) {
    switch (int_num) {
    case INT_DIVIDE_BY_ZERO:
    case INT_OVERFLOW:
    case INT_FLOATING_POINT_EXCEPTION:
    case INT_SIMD_FLOATING_POINT_EXCEPTION:
        return SIGFPE;

    case INT_INVALID_OPCODE:
        return SIGILL;

    case INT_GENERAL_PROTECTION_FAULT:
        return SIGSEGV;

    case INT_PAGE_FAULT:
        return SIGSEGV;

    case INT_SINGLE_STEP:
        // TODO: single stepping
        return 0;

    case INT_BREAKPOINT:
        return SIGTRAP;

    case INT_OUT_OF_BOUNDS:
    case INT_NO_COPROCESSOR:
    case INT_INVALID_TSS:
    case INT_SEGMENT_NOT_PRESENT:
    case INT_STACK_SEGMENT_FAULT:
    case INT_ALIGNMENT_CHECK:
    case INT_MACHINE_CHECK:
    case INT_VIRTUALIZATION_EXCEPTION:
    case INT_CONTROL_PROTECTION_EXCEPTION:
    case INT_HYPERVISOR_INJECTION_EXCEPTION:
    case INT_VMM_COMMUNICATION_EXCEPTION:
    case INT_SECURITY_EXCEPTION:
        return SIGBUS; // sigbus is more of a generic "something went wrong" error

    default:
        return -1; // panic
    }
}

static NORETURN void _crash(int_state* s) {
    panic_prepare();

    log_error("Fatal exception: [int=%#lx | error=%#lx]", s->int_num, s->error_code);

    dump_stack_trace(s->g_regs.rbp);
    dump_regs(s);

    log_fatal("Kernel panic: %s", int_strings[s->int_num]);

    halt();
    __builtin_unreachable();
}

void exception_handler(int_state* s) {
    bool userspace = ((s->s_regs.cs & 3) == 3);
    bool double_fault = (s->int_num == INT_DOUBLE_FAULT);

    // The exception has occurred in userspace
    // Double faults are special, they always panic
    if (userspace && !double_fault) {
        isize signal = _exception_to_signal(s->int_num);

        // Nothing more has to be done
        if (signal == 0)
            return;

        // A valid signal has to be delivered to the process
        if (signal > 1) {
            signal_send(cpu_current_proc(), -1, signal);
            return;
        }
    }

    // If the exception originated in the kernel we are fucked
    _crash(s);
}

void page_fault_handler(int_state* s) {
    // A page fault has occurred while in kernel mode
    if (UNLIKELY(!(s->error_code & PF_USER)))
        _crash(s);

    sched_process* proc = cpu_current_proc();

    if (!proc_handle_page_fault(proc, s))
        exception_handler(s);
}


void dump_regs(int_state* s) {
    gen_regs* g = &s->g_regs;
    spec_regs* r = &s->s_regs;

    log_debug("Dump of machine state: ");
    log_debug("rax=%#016lx rbx=%#016lx rcx=%#016lx", g->rax, g->rbx, g->rcx);
    log_debug("rdx=%#016lx rsi=%#016lx rdi=%#016lx", g->rdx, g->rsi, g->rdi);
    log_debug("rbp=%#016lx r8 =%#016lx r9 =%#016lx", g->rbp, g->r8, g->r9);
    log_debug("r10=%#016lx r11=%#016lx r12=%#016lx", g->r10, g->r11, g->r12);
    log_debug("r13=%#016lx r14=%#016lx r15=%#016lx", g->r13, g->r14, g->r15);
    log_debug("rip=%#016lx rsp=%#016lx flg=%#016lx", r->rip, r->rsp, r->rflags);
    log_debug("cr0=%#016lx cr2=%#016lx cr3=%#016lx", read_cr0(), read_cr2(), read_cr3());
}

void idt_init() {
    for (usize entry = 0; entry < ISR_COUNT; entry++) {
        idt_entry* descriptor = &idt_entries[entry];

        u64 stub_ptr = (u64)isr_stub_table[entry];

        descriptor->offset_low = stub_ptr;
        descriptor->selector = GDT_kernel_code;
        descriptor->attributes = IDT_INT;
        descriptor->offset_mid = stub_ptr >> 16;
        descriptor->offset_high = stub_ptr >> 32;
    }

    idtr.limit = ISR_COUNT * sizeof(idt_entry) - 1;
    idtr.base = (u64)idt_entries;

    // Handle the default x86 exceptions
    for (usize exc = 0; exc < EXCEPTION_COUNT; exc++)
        set_int_handler(exc, exception_handler);

    // Page faults are special
    set_int_handler(INT_PAGE_FAULT, page_fault_handler);

    // Handle all the other possible interrupts
    for (usize trp = EXCEPTION_COUNT; trp < ISR_COUNT; trp++)
        set_int_handler(trp, generic_int_handler);

    asm volatile("lidt %0" ::"m"(idtr) : "memory");
}

void set_int_handler(usize int_num, int_handler handler) {
    assert(int_num < ISR_COUNT);
    assert(handler);

    int_handlers[int_num] = handler;
}

void configure_int(usize int_num, u16 selector, u8 ist, u8 attribs) {
    assert(int_num < ISR_COUNT);

    idt_entry* descriptor = &idt_entries[int_num];

    descriptor->selector = selector;
    descriptor->attributes = attribs;
    descriptor->ist = ist;
}

// Called by isr_common_stub in idt_stubs.asm
void isr_handler(int_state* s) {
    cpu->nest_depth++;

    assert(s->int_num < ISR_COUNT);

    if (cpu->scheduler.running && cpu->nest_depth == 1)
        sched_save(s);

    int_handlers[s->int_num](s);

    cpu->nest_depth--;

    if (cpu->scheduler.running && !cpu->nest_depth) {
        if (cpu->scheduler.needs_resched) {
            schedule();
            cpu->scheduler.needs_resched = false;
        }

        sched_switch();
        __builtin_unreachable();
    }
}
