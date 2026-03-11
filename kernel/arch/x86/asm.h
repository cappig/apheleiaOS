#pragma once

#include <base/attributes.h>
#include <base/types.h>


NORETURN inline void halt(void) {
    for (;;)
        asm("hlt");

    __builtin_unreachable();
}


static inline void swapgs(void) {
    asm volatile("swapgs");
}


static inline void enable_interrupts(void) {
    asm volatile("sti" ::: "memory");
}

static inline void disable_interrupts(void) {
    asm volatile("cli" ::: "memory");
}

static inline void cpu_pause(void) {
    asm volatile("pause");
}

#if defined(__x86_64__)
static inline u64 irq_save(void) {
    u64 flags = 0;

    asm volatile("pushfq\n"
                 "popq %0\n"
                 "cli"
                 : "=r"(flags)
                 :
                 : "memory", "cc");

    return flags;
}

static inline void irq_restore(u64 flags) {
    asm volatile("pushq %0\n"
                 "popfq"
                 :
                 : "r"(flags)
                 : "memory", "cc");
}
#else
static inline u32 irq_save(void) {
    u32 flags = 0;

    asm volatile("pushf\n"
                 "pop %0\n"
                 "cli"
                 : "=r"(flags)
                 :
                 : "memory", "cc");

    return flags;
}

static inline void irq_restore(u32 flags) {
    asm volatile("push %0\n"
                 "popf"
                 :
                 : "r"(flags)
                 : "memory", "cc");
}
#endif


static inline void outb(u16 port, u8 data) {
    asm volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(u16 port, u16 data) {
    asm volatile("outw %0, %1" : : "a"(data), "Nd"(port));
}

static inline u16 inw(u16 port) {
    u16 value = 0;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(u16 port, u32 data) {
    asm volatile("outl %0, %1" : : "a"(data), "Nd"(port));
}

static inline u32 inl(u16 port) {
    u32 value = 0;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}


#if defined(__x86_64__)
static inline void tlb_flush(u64 addr) {
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}
#else
static inline void tlb_flush(u32 addr) {
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}
#endif


#define CR0_EM (1ULL << 2)
#define CR0_TS (1ULL << 3)
#define CR0_MP (1ULL << 1)
#define CR0_WP (1ULL << 16)
#define CR0_PG (1ULL << 31)

#if defined(__x86_64__)
static inline u64 read_cr0(void) {
    u64 value = 0;
    asm volatile("mov %%cr0, %0" : "=r"(value));

    return value;
}

static inline void write_cr0(u64 value) {
    asm volatile("mov %0, %%cr0" : : "r"(value));
}

static inline u64 read_cr2(void) {
    u64 value = 0;
    asm volatile("mov %%cr2, %0" : "=r"(value));

    return value;
}
#else
static inline u32 read_cr0(void) {
    u32 value = 0;
    asm volatile("mov %%cr0, %0" : "=r"(value));

    return value;
}

static inline void write_cr0(u32 value) {
    asm volatile("mov %0, %%cr0" : : "r"(value));
}

static inline u32 read_cr2(void) {
    u32 value = 0;
    asm volatile("mov %%cr2, %0" : "=r"(value));

    return value;
}
#endif

static inline u64 read_cr3(void) {
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
static inline void write_cr3(u64 value) {
    asm volatile("movq %0, %%cr3" : : "r"(value));
}
#else
static inline void write_cr3(u32 value) {
    asm volatile("movl %0, %%cr3" : : "r"(value));
}
#endif

#define CR4_PSE    (1 << 4)
#define CR4_PAE    (1 << 5)
#define CR4_OSFXSR (1 << 9)
#define CR4_OSXMMEXCPT (1 << 10)

#if defined(__x86_64__)
static inline u64 read_cr4(void) {
    u64 value = 0;
    asm volatile("movq %%cr4, %0" : "=r"(value));

    return value;
}

static inline void write_cr4(u64 value) {
    asm volatile("movq %0, %%cr4" : : "r"(value));
}
#else
static inline u32 read_cr4(void) {
    u32 value = 0;
    asm volatile("movl %%cr4, %0" : "=r"(value));

    return value;
}

static inline void write_cr4(u32 value) {
    asm volatile("movl %0, %%cr4" : : "r"(value));
}
#endif


#define CPUID_EXTENDED_INFO 0x80000001
#define CPUID_EI_LM         (1 << 29)
#define CPUID_EI_1G_PAGES   (1 << 26)
#define CPUID_EI_NX         (1 << 20)

typedef struct {
    u32 eax, ebx, ecx, edx;
} cpuid_regs_t;

static inline void cpuid(u32 leaf, cpuid_regs_t *r) {
    asm volatile( // ecx holds the subleaf number
        "cpuid"
        : "=a"(r->eax), "=b"(r->ebx), "=c"(r->ecx), "=d"(r->edx)
        : "a"(leaf), "c"(0)
    );
}


#define EFER_MSR 0xC0000080
#define EFER_NX  (1 << 11)
#define EFER_LME (1 << 8)

#define MSR_PAT 0x277

// PAT memory types
#define PAT_TYPE_UC  0x00
#define PAT_TYPE_WC  0x01
#define PAT_TYPE_WT  0x04
#define PAT_TYPE_WB  0x06
#define PAT_TYPE_UCM 0x07 // UC-

static inline u64 read_msr(u32 msr) {
    u32 edx = 0, eax = 0;
    asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(msr));

    return ((u64)edx << 32 | eax);
}

static inline void write_msr(u32 msr, u64 value) {
    u32 edx = value >> 32;
    u32 eax = value & 0xffffffff;

    asm volatile("wrmsr" : : "a"(eax), "d"(edx), "c"(msr));
}

static inline void pat_init(void) {
    u64 pat = (u64)PAT_TYPE_WB // PAT0: WB  (PCD=0 PWT=0 PAT=0)
              | ((u64)PAT_TYPE_WT << 8) // PAT1: WT  (PCD=0 PWT=1 PAT=0)
              | ((u64)PAT_TYPE_UCM << 16) // PAT2: UC- (PCD=1 PWT=0 PAT=0)
              | ((u64)PAT_TYPE_UC << 24) // PAT3: UC  (PCD=1 PWT=1 PAT=0)
              | ((u64)PAT_TYPE_WC << 32) // PAT4: WC  (PCD=0 PWT=0 PAT=1)
              | ((u64)PAT_TYPE_WT << 40) // PAT5: WT  (PCD=0 PWT=1 PAT=1)
              | ((u64)PAT_TYPE_UCM << 48) // PAT6: UC- (PCD=1 PWT=0 PAT=1)
              | ((u64)PAT_TYPE_UC << 56); // PAT7: UC  (PCD=1 PWT=1 PAT=1)
    write_msr(MSR_PAT, pat);
}

// https://wiki.osdev.org/SWAPGS
#define FS_BASE        0xC0000100
#define GS_BASE        0xC0000101
#define KERNEL_GS_BASE 0xC0000102

static inline void set_gs_base(u64 base) {
    write_msr(KERNEL_GS_BASE, base);
    write_msr(GS_BASE, base);

    swapgs();
}

static inline u64 read_tsc(void) {
    u32 edx = 0, eax = 0;
    asm volatile("rdtsc" : "=a"(eax), "=d"(edx));

    return ((u64)edx << 32 | eax);
}
