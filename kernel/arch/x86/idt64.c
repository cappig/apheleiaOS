// #include <base/attributes.h>
// #include <base/macros.h>
// #include <base/types.h>
// #include <log/log.h>
// #include <x86/asm.h>
// #include <x86/regs.h>
//
// #include "idt.h"
// #include "x86/gdt.h"
//
// static idt_register_t idtr = {0};
//
// extern void* isr_stub_table[ISR_COUNT];
// static int_handler_t int_handlers[ISR_COUNT] = {0};
//
// // ALIGNED(0x10)
// static idt_entry_t idt_entries[ISR_COUNT] = {0};
//
// // "?" indicate a reserved int number
// static const char* int_strings[32] = {
//     "Divide by zero",
//     "Debug",
//     "Non-maskable interrupt",
//     "Breakpoint",
//     "Overflow",
//     "Out of bounds",
//     "Invalid opcode",
//     "No coprocessor",
//     "Double fault",
//     "Coprocessor segover",
//     "Invalid tss",
//     "Segment not present",
//     "Stack segment fault",
//     "General protection fault",
//     "Page fault",
//     "?",
//     "x87 floating point exception",
//     "Alignment check",
//     "Machine check",
//     "SIMD floating point exception",
//     "Virtualization exception",
//     "Control protection exception",
//     "?",
//     "?",
//     "?",
//     "?",
//     "?",
//     "Hypervisor injection exception",
//     "Vmm communication exception",
//     "Security exception",
//     "?",
//     "?",
// };
//
// NORETURN
// static void crash(int_state_t* state) {
//     // panic_prepare();
//
//     log_error("Fatal exception: [int=%#x | error=%#x]", state->int_num, state->error_code);
//
//     // dump_stack_trace(s->g_regs.rbp);
//     // dump_regs(s);
//
//     log_fatal("Kernel panic: %s", int_strings[state->int_num]);
//
//     halt();
//     __builtin_unreachable();
// }
//
// static void generic_int_handler(int_state_t* state) {
//     log_warn("Unhandled interrupt: [int=%#x]", state->int_num);
// }
//
// static void exception_handler(int_state_t* state) {
//     crash(state);
// }
//
// // void dump_regs(int_state* s) {
// //     gen_regs* g = &s->g_regs;
// //     spec_regs* r = &s->s_regs;
// //
// //     log_debug("Dump of machine state: ");
// //     log_debug("rax=%#016lx rbx=%#016lx rcx=%#016lx", g->rax, g->rbx, g->rcx);
// //     log_debug("rdx=%#016lx rsi=%#016lx rdi=%#016lx", g->rdx, g->rsi, g->rdi);
// //     log_debug("rbp=%#016lx r8 =%#016lx r9 =%#016lx", g->rbp, g->r8, g->r9);
// //     log_debug("r10=%#016lx r11=%#016lx r12=%#016lx", g->r10, g->r11, g->r12);
// //     log_debug("r13=%#016lx r14=%#016lx r15=%#016lx", g->r13, g->r14, g->r15);
// //     log_debug("rip=%#016lx rsp=%#016lx flg=%#016lx", r->rip, r->rsp, r->rflags);
// //     log_debug("cr0=%#016lx cr2=%#016lx cr3=%#016lx", read_cr0(), read_cr2(), read_cr3());
// // }
//
// void set_int_handler(size_t int_num, int_handler_t handler) {
//     // assert(int_num < ISR_COUNT);
//     // assert(handler);
//
//     int_handlers[int_num] = handler;
// }
//
// void configure_int(size_t int_num, u16 selector, u8 ist, u8 attribs) {
//     // assert(int_num < ISR_COUNT);
//
//     idt_entry_t* descriptor = &idt_entries[int_num];
//
//     descriptor->selector = selector;
//     descriptor->attributes = attribs;
//     descriptor->ist = ist;
// }
//
// void idt_init() {
//     for (size_t entry = 0; entry < ISR_COUNT; entry++) {
//         idt_entry_t* descriptor = &idt_entries[entry];
//
//         u64 stub_ptr = (u64)isr_stub_table[entry];
//
//         descriptor->offset_low = stub_ptr;
//         descriptor->selector = GDT_KERNEL_CODE;
//         descriptor->attributes = IDT_INT;
//         descriptor->offset_mid = stub_ptr >> 16;
//         descriptor->offset_high = stub_ptr >> 32;
//     }
//
//     log_debug("IDT initialized");
//
//     idtr.limit = ISR_COUNT * sizeof(idt_entry_t) - 1;
//     idtr.base = (u64)idt_entries;
//
//
//     // Handle the default x86 exceptions
//     for (size_t exc = 0; exc < EXCEPTION_COUNT; exc++)
//         set_int_handler(exc, exception_handler);
//
//     // Page faults are special
//     // set_int_handler(INT_PAGE_FAULT, page_fault_handler);
//
//     // Handle all the other possible interrupts
//     for (size_t trp = EXCEPTION_COUNT; trp < ISR_COUNT; trp++)
//         set_int_handler(trp, generic_int_handler);
//
//     asm volatile("lidt %0" ::"m"(idtr) : "memory");
// }
//
// // Called by isr_common_stub in idt_stubs.asm
// void isr_handler(int_state_t* state) {
//     // cpu->nest_depth++;
//
//     // assert(s->int_num < ISR_COUNT);
//
//     // if (cpu->scheduler.running && cpu->nest_depth == 1)
//     //     sched_save(s);
//     //
//
//     int_handler_t handler = int_handlers[state->int_num];
//
//     handler(state);
//
//     // cpu->nest_depth--;
//
//     // if (cpu->scheduler.running && !cpu->nest_depth) {
//     //     if (cpu->scheduler.needs_resched) {
//     //         schedule();
//     //         cpu->scheduler.needs_resched = false;
//     //     }
//     //
//     //     sched_switch();
//     //     __builtin_unreachable();
//     // }
// }

#include "idt.h"
#include "x86/asm.h"

void isr_handler(int_state_t* state) {
    (void)state;

    // Temporary minimal handler to keep early boot builds linkable.
    // Once the IDT is fully wired, this should dispatch to registered handlers.
    disable_interrupts();
    halt();
}
