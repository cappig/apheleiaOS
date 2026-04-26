#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define SSTATUS_SIE  (1UL << 1)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SPP  (1UL << 8)
#define SSTATUS_SUM  (1UL << 18)

#define MSTATUS_MIE      (1UL << 3)
#define MSTATUS_MPIE     (1UL << 7)
#define MSTATUS_MPP_MASK (3UL << 11)
#define MSTATUS_MPP_S    (1UL << 11)

#define MIE_MSIE (1UL << 3)
#define MIE_MTIE (1UL << 7)
#define MIE_MEIE (1UL << 11)

#define SIE_SSIE (1UL << 1)
#define SIE_STIE (1UL << 5)
#define SIE_SEIE (1UL << 9)

#define MIP_SSIP (1UL << 1)
#define MIP_STIP (1UL << 5)
#define MIP_MTIP (1UL << 7)

#define SIP_SSIP (1UL << 1)

#define RISCV_COUNTEREN_CY (1UL << 0)
#define RISCV_COUNTEREN_TM (1UL << 1)
#define RISCV_COUNTEREN_IR (1UL << 2)

#define RISCV_MCALL_EXT_TIMER       0x4154494dUL
#define RISCV_MCALL_TIMER_ARM_DELTA 0UL
#define RISCV_MCALL_TIMER_READ_TIME 1UL

#define PMP_R       0x01U
#define PMP_W       0x02U
#define PMP_X       0x04U
#define PMP_A_NAPOT 0x18U

NORETURN static inline void halt(void) {
    for (;;) {
        asm volatile("wfi");
    }

    __builtin_unreachable();
}

static inline void mmio_fence(void) {
    asm volatile("fence iorw, iorw" ::: "memory");
}

static inline void sfence_vma(void) {
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

static inline unsigned long riscv_read_sstatus(void) {
    unsigned long value = 0;
    asm volatile("csrr %0, sstatus" : "=r"(value));
    return value;
}

static inline unsigned long riscv_read_mstatus(void) {
    unsigned long value = 0;
    asm volatile("csrr %0, mstatus" : "=r"(value));
    return value;
}

static inline void riscv_write_sstatus(unsigned long value) {
    asm volatile("csrw sstatus, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_mstatus(unsigned long value) {
    asm volatile("csrw mstatus, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_mscratch(uintptr_t value) {
    asm volatile("csrw mscratch, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_sscratch(uintptr_t value) {
    asm volatile("csrw sscratch, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_mtvec(uintptr_t value) {
    asm volatile("csrw mtvec, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_stvec(uintptr_t value) {
    asm volatile("csrw stvec, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_mepc(uintptr_t value) {
    asm volatile("csrw mepc, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_medeleg(unsigned long value) {
    asm volatile("csrw medeleg, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_mideleg(unsigned long value) {
    asm volatile("csrw mideleg, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_mie(unsigned long value) {
    asm volatile("csrw mie, %0" : : "r"(value) : "memory");
}

static inline void riscv_set_mie_bits(unsigned long value) {
    asm volatile("csrs mie, %0" : : "r"(value) : "memory");
}

static inline void riscv_clear_mie_bits(unsigned long value) {
    asm volatile("csrc mie, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_mcounteren(unsigned long value) {
    asm volatile("csrw mcounteren, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_sie(unsigned long value) {
    asm volatile("csrw sie, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_scounteren(unsigned long value) {
    asm volatile("csrw scounteren, %0" : : "r"(value) : "memory");
}

static inline void riscv_set_mip_bits(unsigned long value) {
    asm volatile("csrs mip, %0" : : "r"(value) : "memory");
}

static inline void riscv_clear_mip_bits(unsigned long value) {
    asm volatile("csrc mip, %0" : : "r"(value) : "memory");
}

static inline void riscv_set_sie_bits(unsigned long value) {
    asm volatile("csrs sie, %0" : : "r"(value) : "memory");
}

static inline void riscv_clear_sie_bits(unsigned long value) {
    asm volatile("csrc sie, %0" : : "r"(value) : "memory");
}

static inline void riscv_set_sip_bits(unsigned long value) {
    asm volatile("csrs sip, %0" : : "r"(value) : "memory");
}

static inline void riscv_clear_sip_bits(unsigned long value) {
    asm volatile("csrc sip, %0" : : "r"(value) : "memory");
}

static inline void riscv_enable_irqs(void) {
    unsigned long bits = SSTATUS_SIE;
    asm volatile("csrs sstatus, %0" : : "r"(bits) : "memory");
}

static inline void riscv_disable_irqs(void) {
    unsigned long bits = SSTATUS_SIE;
    asm volatile("csrc sstatus, %0" : : "r"(bits) : "memory");
}

static inline u64 riscv_read_time(void) {
#if __riscv_xlen == 64
    u64 value = 0;
    asm volatile("rdtime %0" : "=r"(value));
    return value;
#else
    u32 hi = 0;
    u32 lo = 0;
    u32 hi2 = 0;

    do {
        asm volatile("rdtimeh %0" : "=r"(hi));
        asm volatile("rdtime %0" : "=r"(lo));
        asm volatile("rdtimeh %0" : "=r"(hi2));
    } while (hi != hi2);

    return ((u64)hi << 32) | lo;
#endif
}

static inline uintptr_t riscv_read_tp(void) {
    uintptr_t value = 0;
    asm volatile("mv %0, tp" : "=r"(value));
    return value;
}

static inline void riscv_write_tp(uintptr_t value) {
    asm volatile("mv tp, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_pmpaddr0(uintptr_t value) {
    asm volatile("csrw pmpaddr0, %0" : : "r"(value) : "memory");
}

static inline void riscv_write_pmpcfg0(unsigned long value) {
    asm volatile("csrw pmpcfg0, %0" : : "r"(value) : "memory");
}

static inline bool riscv_machine_timer_arm_delta(u64 delta) {
#if __riscv_xlen == 64
    register unsigned long a0 asm("a0") = (unsigned long)delta;
    register unsigned long a1 asm("a1") = 0;
#else
    register unsigned long a0 asm("a0") = (u32)delta;
    register unsigned long a1 asm("a1") = (u32)(delta >> 32);
#endif
    register unsigned long a6 asm("a6") = RISCV_MCALL_TIMER_ARM_DELTA;
    register unsigned long a7 asm("a7") = RISCV_MCALL_EXT_TIMER;

    asm volatile(
        "ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a6), "r"(a7)
        : "memory"
    );

    return (long)a0 == 0;
}

static inline bool riscv_machine_timer_read(u64 *value) {
    if (!value) {
        return false;
    }

#if __riscv_xlen == 64
    register unsigned long a0 asm("a0") = 0;
    register unsigned long a1 asm("a1") = 0;
    register unsigned long a2 asm("a2") = 0;
#else
    register unsigned long a0 asm("a0") = 0;
    register unsigned long a1 asm("a1") = 0;
    register unsigned long a2 asm("a2") = 0;
#endif
    register unsigned long a6 asm("a6") = RISCV_MCALL_TIMER_READ_TIME;
    register unsigned long a7 asm("a7") = RISCV_MCALL_EXT_TIMER;

    asm volatile(
        "ecall"
        : "+r"(a0), "+r"(a1), "+r"(a2)
        : "r"(a6), "r"(a7)
        : "memory"
    );

    if ((long)a0 != 0) {
        return false;
    }

#if __riscv_xlen == 64
    *value = (u64)a1;
#else
    *value = ((u64)a2 << 32) | (u32)a1;
#endif

    return true;
}

static inline void riscv_write_satp(uintptr_t root, u64 mode) {
#if __riscv_xlen == 64
    u64 value = (mode << 60) | ((u64)root >> 12);
    asm volatile("csrw satp, %0" : : "r"(value) : "memory");
#else
    u32 value = (u32)((mode << 31) | ((u32)root >> 12));
    asm volatile("csrw satp, %0" : : "r"(value) : "memory");
#endif
    sfence_vma();
}
