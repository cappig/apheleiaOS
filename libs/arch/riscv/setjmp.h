#pragma once

#if __riscv_xlen != 32 && __riscv_xlen != 64
#error "Unsupported RISC-V XLEN"
#endif

typedef unsigned long arch_jmp_buf_t[14];

#ifndef __TINYC__
__attribute__((always_inline)) static inline int arch_setjmp(arch_jmp_buf_t env) {
    register int ret asm("a0");

#if __riscv_xlen == 64
    asm volatile("lla t0, 0f\n\t"
                 "sd t0, 0(%1)\n\t"
                 "sd s0, 8(%1)\n\t"
                 "sd s1, 16(%1)\n\t"
                 "sd s2, 24(%1)\n\t"
                 "sd s3, 32(%1)\n\t"
                 "sd s4, 40(%1)\n\t"
                 "sd s5, 48(%1)\n\t"
                 "sd s6, 56(%1)\n\t"
                 "sd s7, 64(%1)\n\t"
                 "sd s8, 72(%1)\n\t"
                 "sd s9, 80(%1)\n\t"
                 "sd s10, 88(%1)\n\t"
                 "sd s11, 96(%1)\n\t"
                 "sd sp, 104(%1)\n\t"
                 "li a0, 0\n\t"
                 "0:\n\t"
                 : "=r"(ret)
                 : "r"(env)
                 : "memory", "t0");
#else
    asm volatile("lla t0, 0f\n\t"
                 "sw t0, 0(%1)\n\t"
                 "sw s0, 4(%1)\n\t"
                 "sw s1, 8(%1)\n\t"
                 "sw s2, 12(%1)\n\t"
                 "sw s3, 16(%1)\n\t"
                 "sw s4, 20(%1)\n\t"
                 "sw s5, 24(%1)\n\t"
                 "sw s6, 28(%1)\n\t"
                 "sw s7, 32(%1)\n\t"
                 "sw s8, 36(%1)\n\t"
                 "sw s9, 40(%1)\n\t"
                 "sw s10, 44(%1)\n\t"
                 "sw s11, 48(%1)\n\t"
                 "sw sp, 52(%1)\n\t"
                 "li a0, 0\n\t"
                 "0:\n\t"
                 : "=r"(ret)
                 : "r"(env)
                 : "memory", "t0");
#endif

    return ret;
}

__attribute__((always_inline, noreturn)) static inline void arch_longjmp(arch_jmp_buf_t env, int val) {
    if (!val) {
        val = 1;
    }

#if __riscv_xlen == 64
    asm volatile("mv t0, %0\n\t"
                 "mv t1, %1\n\t"
                 "ld ra, 0(t0)\n\t"
                 "ld s0, 8(t0)\n\t"
                 "ld s1, 16(t0)\n\t"
                 "ld s2, 24(t0)\n\t"
                 "ld s3, 32(t0)\n\t"
                 "ld s4, 40(t0)\n\t"
                 "ld s5, 48(t0)\n\t"
                 "ld s6, 56(t0)\n\t"
                 "ld s7, 64(t0)\n\t"
                 "ld s8, 72(t0)\n\t"
                 "ld s9, 80(t0)\n\t"
                 "ld s10, 88(t0)\n\t"
                 "ld s11, 96(t0)\n\t"
                 "ld sp, 104(t0)\n\t"
                 "mv a0, t1\n\t"
                 "jr ra\n\t"
                 :
                 : "r"(env), "r"(val)
                 : "memory", "a0", "t0", "t1");
#else
    asm volatile("mv t0, %0\n\t"
                 "mv t1, %1\n\t"
                 "lw ra, 0(t0)\n\t"
                 "lw s0, 4(t0)\n\t"
                 "lw s1, 8(t0)\n\t"
                 "lw s2, 12(t0)\n\t"
                 "lw s3, 16(t0)\n\t"
                 "lw s4, 20(t0)\n\t"
                 "lw s5, 24(t0)\n\t"
                 "lw s6, 28(t0)\n\t"
                 "lw s7, 32(t0)\n\t"
                 "lw s8, 36(t0)\n\t"
                 "lw s9, 40(t0)\n\t"
                 "lw s10, 44(t0)\n\t"
                 "lw s11, 48(t0)\n\t"
                 "lw sp, 52(t0)\n\t"
                 "mv a0, t1\n\t"
                 "jr ra\n\t"
                 :
                 : "r"(env), "r"(val)
                 : "memory", "a0", "t0", "t1");
#endif

    __builtin_unreachable();
}
#endif
