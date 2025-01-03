#pragma once

#include <base/types.h>


inline void halt(void) {
    for (;;)
        asm("hlt");

    __builtin_unreachable();
}


inline void swapgs(void) {
    asm volatile("swapgs");
}


inline void enable_interrupts(void) {
    asm volatile("sti" ::: "memory");
}

inline void disable_interrupts(void) {
    asm volatile("cli" ::: "memory");
}


inline void outb(u16 port, u8 data) {
    asm volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

inline u8 inb(u16 port) {
    u8 value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outw(u16 port, u16 data) {
    asm volatile("outw %0, %1" : : "a"(data), "Nd"(port));
}

inline u16 inw(u16 port) {
    u16 value = 0;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outl(u16 port, u32 data) {
    asm volatile("outl %0, %1" : : "a"(data), "Nd"(port));
}

inline u32 inl(u16 port) {
    u32 value = 0;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}


inline void tlb_flush(u64 addr) {
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}


#define CR0_WP (1ULL << 16)
#define CR0_PG (1ULL << 31)

inline u64 read_cr0(void) {
    u64 value = 0;
    asm volatile("mov %%cr0, %0" : "=r"(value));

    return value;
}

inline void write_cr0(u32 value) {
    asm volatile("mov %0, %%cr0" : : "r"(value));
}

inline u64 read_cr2(void) {
    u64 value = 0;
    asm volatile("mov %%cr2, %0" : "=r"(value));

    return value;
}

inline u64 read_cr3(void) {
#if defined(__x86_64__)
    u64 value = 0;
    asm volatile("movq %%cr3, %0" : "=r"(value));
#else
    u32 value = 0;
    asm volatile("movl %%cr3, %0" : "=r"(value));
#endif

    return value;
}


#if defined(__x86_64__)
inline void write_cr3(u64 value) {
    asm volatile("movq %0, %%cr3" : : "r"(value));
}
#else
inline void write_cr3(u32 value) {
    asm volatile("movl %0, %%cr3" : : "r"(value));
}
#endif

#define CR4_PAE (1 << 5)

inline u64 read_cr4(void) {
    u32 value = 0;
    asm volatile("movl %%cr4, %0" : "=r"(value));

    return value;
}

inline void write_cr4(u32 value) {
    asm volatile("movl %0, %%cr4" : : "r"(value));
}


#define CPUID_EXTENDED_INFO 0x80000001
#define CPUID_EI_LM         (1 << 29)
#define CPUID_EI_1G_PAGES   (1 << 26)

typedef struct {
    u32 eax, ebx, ecx, edx;
} cpuid_regs;

inline void cpuid(u32 leaf, cpuid_regs* r) {
    asm volatile( // ecx holds the subleaf number
        "cpuid"
        : "=a"(r->eax), "=b"(r->ebx), "=c"(r->ecx), "=d"(r->edx)
        : "a"(leaf), "c"(0)
    );
}


#define EFER_MSR 0xC0000080
#define EFER_NX  (1 << 11)
#define EFER_LME (1 << 8)

inline u64 read_msr(u32 msr) {
    u32 edx = 0, eax = 0;
    asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(msr));

    return ((u64)edx << 32 | eax);
}

inline void write_msr(u32 msr, u64 value) {
    u32 edx = value >> 32;
    u32 eax = value & 0xffffffff;

    asm volatile("wrmsr" : : "a"(eax), "d"(edx), "c"(msr));
}


inline u64 read_tsc(void) {
    u32 edx = 0, eax = 0;
    asm volatile("rdtsc" : "=a"(eax), "=d"(edx));

    return ((u64)edx << 32 | eax);
}
