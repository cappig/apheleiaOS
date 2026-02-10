#pragma once

#include <stdint.h>

#define SYSCALL_INT 0x80

#if defined(__x86_64__)
static inline uint64_t syscall0(uint64_t num) {
    uint64_t ret = num;
    asm volatile("int $" "0x80" : "=a"(ret) : "a"(ret) : "memory");
    return ret;
}

static inline uint64_t syscall1(uint64_t num, uint64_t arg1) {
    uint64_t ret = num;
    asm volatile("int $" "0x80" : "=a"(ret) : "a"(ret), "D"(arg1) : "memory");
    return ret;
}

static inline uint64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    uint64_t ret = num;
    asm volatile("int $" "0x80" : "=a"(ret) : "a"(ret), "D"(arg1), "S"(arg2) : "memory");
    return ret;
}

static inline uint64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint64_t ret = num;
    asm volatile(
        "int $"
        "0x80"
        : "=a"(ret)
        : "a"(ret), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline uint64_t
syscall4(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
    uint64_t ret = num;
    register uint64_t r10 asm("r10") = arg4;
    asm volatile(
        "int $"
        "0x80"
        : "=a"(ret)
        : "a"(ret), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10)
        : "memory"
    );
    return ret;
}
#elif defined(__i386__)
static inline uint32_t syscall0(uint32_t num) {
    uint32_t ret = num;
    asm volatile("int $" "0x80" : "=a"(ret) : "a"(ret) : "memory");
    return ret;
}

static inline uint32_t syscall1(uint32_t num, uint32_t arg1) {
    uint32_t ret = num;
    asm volatile("int $" "0x80" : "=a"(ret) : "a"(ret), "b"(arg1) : "memory");
    return ret;
}

static inline uint32_t syscall2(uint32_t num, uint32_t arg1, uint32_t arg2) {
    uint32_t ret = num;
    asm volatile("int $" "0x80" : "=a"(ret) : "a"(ret), "b"(arg1), "c"(arg2) : "memory");
    return ret;
}

static inline uint32_t syscall3(uint32_t num, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t ret = num;
    asm volatile(
        "int $"
        "0x80"
        : "=a"(ret)
        : "a"(ret), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline uint32_t
syscall4(uint32_t num, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    uint32_t ret = num;
    asm volatile(
        "int $"
        "0x80"
        : "=a"(ret)
        : "a"(ret), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4)
        : "memory"
    );
    return ret;
}
#else
#error "Unsupported architecture"
#endif
