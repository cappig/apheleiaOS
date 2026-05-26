#pragma once

#if defined(__x86_64__)
typedef unsigned long arch_jmp_buf_t[8];

__attribute__((always_inline)) static inline int arch_setjmp(arch_jmp_buf_t env) {
    int ret;

    asm volatile("movq %%rbx, 0(%1)\n\t"
                 "movq %%rbp, 8(%1)\n\t"
                 "movq %%r12, 16(%1)\n\t"
                 "movq %%r13, 24(%1)\n\t"
                 "movq %%r14, 32(%1)\n\t"
                 "movq %%r15, 40(%1)\n\t"
                 "leaq 0f(%%rip), %%rax\n\t"
                 "movq %%rax, 48(%1)\n\t"
                 "movq %%rsp, 56(%1)\n\t"
                 "xorl %%eax, %%eax\n\t"
                 "0:\n\t"
                 : "=&a"(ret)
                 : "r"(env)
                 : "memory");

    return ret;
}

__attribute__((always_inline, noreturn)) static inline void arch_longjmp(arch_jmp_buf_t env, int val) {
    if (!val) {
        val = 1;
    }

    asm volatile("movq %0, %%rdx\n\t"
                 "movl %1, %%eax\n\t"
                 "movq 0(%%rdx), %%rbx\n\t"
                 "movq 8(%%rdx), %%rbp\n\t"
                 "movq 16(%%rdx), %%r12\n\t"
                 "movq 24(%%rdx), %%r13\n\t"
                 "movq 32(%%rdx), %%r14\n\t"
                 "movq 40(%%rdx), %%r15\n\t"
                 "movq 56(%%rdx), %%rsp\n\t"
                 "jmp *48(%%rdx)\n\t"
                 :
                 : "r"(env), "r"(val)
                 : "memory", "rax", "rdx");

    __builtin_unreachable();
}
#elif defined(__i386__)
typedef unsigned int arch_jmp_buf_t[6];

__attribute__((always_inline)) static inline int arch_setjmp(arch_jmp_buf_t env) {
    int ret;

    asm volatile("movl %%ebx, 0(%1)\n\t"
                 "movl %%esi, 4(%1)\n\t"
                 "movl %%edi, 8(%1)\n\t"
                 "movl %%ebp, 12(%1)\n\t"
                 "movl $0f, %%eax\n\t"
                 "movl %%eax, 16(%1)\n\t"
                 "movl %%esp, 20(%1)\n\t"
                 "xorl %%eax, %%eax\n\t"
                 "0:\n\t"
                 : "=&a"(ret)
                 : "r"(env)
                 : "memory");

    return ret;
}

__attribute__((always_inline, noreturn)) static inline void arch_longjmp(arch_jmp_buf_t env, int val) {
    if (!val) {
        val = 1;
    }

    asm volatile("movl %0, %%edx\n\t"
                 "movl %1, %%eax\n\t"
                 "movl 0(%%edx), %%ebx\n\t"
                 "movl 4(%%edx), %%esi\n\t"
                 "movl 8(%%edx), %%edi\n\t"
                 "movl 12(%%edx), %%ebp\n\t"
                 "movl 20(%%edx), %%esp\n\t"
                 "jmp *16(%%edx)\n\t"
                 :
                 : "r"(env), "r"(val)
                 : "memory", "eax", "edx");

    __builtin_unreachable();
}
#else
#error "Unsupported architecture"
#endif
