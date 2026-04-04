#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <riscv/trap.h>

typedef struct PACKED {
    uintptr_t mepc;
    uintptr_t mstatus;
    uintptr_t sp;
    uintptr_t mcause;
    uintptr_t mtval;
    uintptr_t mhartid;
} riscv_mreg_state_t;

typedef struct PACKED {
    riscv_gpr_state_t g_regs;
    riscv_mreg_state_t m_regs;
} riscv_mtrap_frame_t;

void riscv_boot_handle_trap(riscv_mtrap_frame_t *frame);
extern void riscv_boot_mtrap_entry(void);
