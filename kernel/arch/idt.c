#include "idt.h"

#include <base/attributes.h>
#include <base/types.h>
#include <log/log.h>
#include <x86/asm.h>
#include <x86/regs.h>

#include "panic.h"
#include "pic.h"

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

static void exception_handler(int_state* s) {
    disble_interrupts();
    dump_regs(s);
    // TODO: implement this for real
    // dump_stack_trace(s->g_regs.rbp);

    log_fatal("Unhandled exception: [int=%#lx | error=%#lx]", s->int_num, s->error_code);
    panic("Kernel panic: %s", int_strings[s->int_num]);
}

void idt_init() {
    for (usize vector = 0; vector < ISR_COUNT; vector++) {
        idt_entry* descriptor = &idt_entries[vector];

        u64 stub_ptr = (u64)isr_stub_table[vector];

        descriptor->offset_low = stub_ptr;
        descriptor->selector = 0x08;
        descriptor->attributes = 0x8e;
        descriptor->offset_mid = stub_ptr >> 16;
        descriptor->offset_high = stub_ptr >> 32;
    }

    idtr.limit = ISR_COUNT * sizeof(idt_entry) - 1;
    idtr.base = (u64)idt_entries;

    // Handle the default x86 exceptions
    for (usize exc = 0; exc < INT_COUNT; exc++)
        set_int_handler(exc, exception_handler);

    // Handle all the other possible interrupts
    for (usize trp = INT_COUNT; trp < ISR_COUNT; trp++)
        set_int_handler(trp, generic_int_handler);

    asm volatile("lidt %0" ::"m"(idtr) : "memory");
}

void set_int_handler(usize int_num, int_handler handler) {
    int_handlers[int_num] = handler;
}

// Called by isr_common_stub in idt_stubs.asm
void isr_handler(int_state* s) {
    if (s->int_num >= ISR_COUNT)
        panic("Unknown interrupt number [int=%#lx]", s->int_num);

    if (IS_IRQ(s->int_num) && s->int_num != IRQ_SPURIOUS)
        pic_end_int(s->int_num - IRQ_OFFSET);

    int_handlers[s->int_num](s);
}
