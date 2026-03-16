#pragma once

#include <stdint.h>

#if __riscv_xlen == 64
typedef uint64_t riscv_word_t;
#elif __riscv_xlen == 32
typedef uint32_t riscv_word_t;
#else
#error "Unsupported RISC-V XLEN"
#endif

static inline riscv_word_t syscall0(riscv_word_t num) {
    register riscv_word_t a0 asm("a0") = 0;
    register riscv_word_t a7 asm("a7") = num;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline riscv_word_t syscall1(riscv_word_t num, riscv_word_t arg1) {
    register riscv_word_t a0 asm("a0") = arg1;
    register riscv_word_t a7 asm("a7") = num;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline riscv_word_t
syscall2(riscv_word_t num, riscv_word_t arg1, riscv_word_t arg2) {
    register riscv_word_t a0 asm("a0") = arg1;
    register riscv_word_t a1 asm("a1") = arg2;
    register riscv_word_t a7 asm("a7") = num;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline riscv_word_t syscall3(
    riscv_word_t num,
    riscv_word_t arg1,
    riscv_word_t arg2,
    riscv_word_t arg3
) {
    register riscv_word_t a0 asm("a0") = arg1;
    register riscv_word_t a1 asm("a1") = arg2;
    register riscv_word_t a2 asm("a2") = arg3;
    register riscv_word_t a7 asm("a7") = num;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline riscv_word_t syscall4(
    riscv_word_t num,
    riscv_word_t arg1,
    riscv_word_t arg2,
    riscv_word_t arg3,
    riscv_word_t arg4
) {
    register riscv_word_t a0 asm("a0") = arg1;
    register riscv_word_t a1 asm("a1") = arg2;
    register riscv_word_t a2 asm("a2") = arg3;
    register riscv_word_t a3 asm("a3") = arg4;
    register riscv_word_t a7 asm("a7") = num;
    asm volatile(
        "ecall"
        : "+r"(a0)
        : "r"(a1), "r"(a2), "r"(a3), "r"(a7)
        : "memory"
    );
    return a0;
}
