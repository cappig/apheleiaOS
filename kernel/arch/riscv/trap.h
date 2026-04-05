#pragma once

#include <base/attributes.h>
#include <base/types.h>

typedef struct PACKED {
    uintptr_t ra;
    uintptr_t gp;
    uintptr_t tp;
    uintptr_t t0;
    uintptr_t t1;
    uintptr_t t2;
    uintptr_t s0;
    uintptr_t s1;
    uintptr_t a0;
    uintptr_t a1;
    uintptr_t a2;
    uintptr_t a3;
    uintptr_t a4;
    uintptr_t a5;
    uintptr_t a6;
    uintptr_t a7;
    uintptr_t s2;
    uintptr_t s3;
    uintptr_t s4;
    uintptr_t s5;
    uintptr_t s6;
    uintptr_t s7;
    uintptr_t s8;
    uintptr_t s9;
    uintptr_t s10;
    uintptr_t s11;
    uintptr_t t3;
    uintptr_t t4;
    uintptr_t t5;
    uintptr_t t6;
} riscv_gpr_state_t;

typedef struct PACKED {
    uintptr_t sepc;
    uintptr_t sstatus;
    uintptr_t sp;
    uintptr_t scause;
    uintptr_t stval;
} riscv_sreg_state_t;

typedef struct PACKED {
    riscv_gpr_state_t g_regs;
    riscv_sreg_state_t s_regs;
} arch_int_state_t;

void riscv_trap_init(void);
void riscv_handle_trap(arch_int_state_t *state);
void riscv_resched_trap(void);

extern uintptr_t riscv_kernel_sp;
extern uintptr_t riscv_cpu_local_ptr;
