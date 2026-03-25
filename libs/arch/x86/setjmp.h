#pragma once

#if defined(__x86_64__)
typedef unsigned long arch_jmp_buf_t[8];

static inline int arch_setjmp(arch_jmp_buf_t env) {
    asm volatile(
        "movq %%rbx, 0(%0)\n\t"
        "movq %%rbp, 8(%0)\n\t"
        "movq %%r12, 16(%0)\n\t"
        "movq %%r13, 24(%0)\n\t"
        "movq %%r14, 32(%0)\n\t"
        "movq %%r15, 40(%0)\n\t"
        "movq (%%rsp), %%rax\n\t"
        "movq %%rax, 48(%0)\n\t"
        "leaq 8(%%rsp), %%rax\n\t"
        "movq %%rax, 56(%0)\n\t"
        :
        : "r"(env)
        : "memory", "rax"
    );

    return 0;
}

__attribute__((noreturn)) static inline void
arch_longjmp(arch_jmp_buf_t env, int val) {
    if (!val) {
        val = 1;
    }

    asm volatile(
        "movl %1, %%eax\n\t"
        "movq 0(%0), %%rbx\n\t"
        "movq 8(%0), %%rbp\n\t"
        "movq 16(%0), %%r12\n\t"
        "movq 24(%0), %%r13\n\t"
        "movq 32(%0), %%r14\n\t"
        "movq 40(%0), %%r15\n\t"
        "movq 56(%0), %%rsp\n\t"
        "jmp *48(%0)\n\t"
        :
        : "r"(env), "r"(val)
        : "memory", "rax"
    );

    __builtin_unreachable();
}
#elif defined(__i386__)
typedef unsigned int arch_jmp_buf_t[6];

static inline int arch_setjmp(arch_jmp_buf_t env) {
    asm volatile(
        "movl %%ebx, 0(%0)\n\t"
        "movl %%esi, 4(%0)\n\t"
        "movl %%edi, 8(%0)\n\t"
        "movl %%ebp, 12(%0)\n\t"
        "movl (%%esp), %%eax\n\t"
        "movl %%eax, 16(%0)\n\t"
        "leal 4(%%esp), %%eax\n\t"
        "movl %%eax, 20(%0)\n\t"
        :
        : "r"(env)
        : "memory", "eax"
    );

    return 0;
}

__attribute__((noreturn)) static inline void
arch_longjmp(arch_jmp_buf_t env, int val) {
    if (!val) {
        val = 1;
    }

    asm volatile(
        "movl %1, %%eax\n\t"
        "movl 0(%0), %%ebx\n\t"
        "movl 4(%0), %%esi\n\t"
        "movl 8(%0), %%edi\n\t"
        "movl 12(%0), %%ebp\n\t"
        "movl 20(%0), %%esp\n\t"
        "jmp *16(%0)\n\t"
        :
        : "r"(env), "r"(val)
        : "memory", "eax"
    );

    __builtin_unreachable();
}
#else
#error "Unsupported architecture"
#endif
