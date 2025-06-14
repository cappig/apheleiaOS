#pragma once

#include <base/macros.h>
#include <base/types.h>

#define SYSCALL_INT 0x80

#define _SYS_ASM "int $" STR(SYSCALL_INT)


inline u64 syscall0(u64 num) {
    u64 ret = num;
    asm volatile(_SYS_ASM : "=a"(ret) : "a"(ret) : "memory");

    return ret;
}

inline u64 syscall1(u64 num, u64 arg1) {
    u64 ret = num;
    asm volatile(_SYS_ASM : "=a"(ret) : "a"(ret), "D"(arg1) : "memory");

    return ret;
}

inline u64 syscall2(u64 num, u64 arg1, u64 arg2) {
    u64 ret = num;
    asm volatile(_SYS_ASM : "=a"(ret) : "a"(ret), "D"(arg1), "S"(arg2) : "memory");

    return ret;
}

inline u64 syscall3(u64 num, u64 arg1, u64 arg2, u64 arg3) {
    u64 ret = num;
    asm volatile(_SYS_ASM : "=a"(ret) : "a"(ret), "D"(arg1), "S"(arg2), "d"(arg3) : "memory");

    return ret;
}

inline u64 syscall4(u64 num, u64 arg1, u64 arg2, u64 arg3, u64 arg4) {
    u64 ret = num;
    asm volatile(_SYS_ASM
                 : "=a"(ret)
                 : "a"(ret), "D"(arg1), "S"(arg2), "d"(arg3), "c"(arg4)
                 : "memory");

    return ret;
}

inline u64 syscall5(u64 num, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5) {
    register u64 r8 asm("r8") = arg5;

    u64 ret = num;
    asm volatile(_SYS_ASM
                 : "=a"(ret)
                 : "a"(ret), "D"(arg1), "S"(arg2), "d"(arg3), "c"(arg4), "r"(r8)
                 : "memory");

    return ret;
}

inline u64 syscall6(u64 num, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5, u64 arg6) {
    register u64 r8 asm("r8") = (u64)arg5;
    register u64 r9 asm("r9") = (u64)arg6;

    u64 ret = num;
    asm volatile(_SYS_ASM
                 : "=a"(ret)
                 : "a"(ret), "D"(arg1), "S"(arg2), "d"(arg3), "c"(arg4), "r"(r8), "r"(r9)
                 : "memory");

    return ret;
}
