#include <setjmp.h>

int setjmp(jmp_buf env) {
#if defined(__x86_64__)
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
#elif defined(__i386__)
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
#elif defined(__riscv)
#if __riscv_xlen == 64
    asm volatile(
        "sd ra, 0(%0)\n\t"
        "sd s0, 8(%0)\n\t"
        "sd s1, 16(%0)\n\t"
        "sd s2, 24(%0)\n\t"
        "sd s3, 32(%0)\n\t"
        "sd s4, 40(%0)\n\t"
        "sd s5, 48(%0)\n\t"
        "sd s6, 56(%0)\n\t"
        "sd s7, 64(%0)\n\t"
        "sd s8, 72(%0)\n\t"
        "sd s9, 80(%0)\n\t"
        "sd s10, 88(%0)\n\t"
        "sd s11, 96(%0)\n\t"
        "sd sp, 104(%0)\n\t"
        :
        : "r"(env)
        : "memory"
    );
#else
    asm volatile(
        "sw ra, 0(%0)\n\t"
        "sw s0, 4(%0)\n\t"
        "sw s1, 8(%0)\n\t"
        "sw s2, 12(%0)\n\t"
        "sw s3, 16(%0)\n\t"
        "sw s4, 20(%0)\n\t"
        "sw s5, 24(%0)\n\t"
        "sw s6, 28(%0)\n\t"
        "sw s7, 32(%0)\n\t"
        "sw s8, 36(%0)\n\t"
        "sw s9, 40(%0)\n\t"
        "sw s10, 44(%0)\n\t"
        "sw s11, 48(%0)\n\t"
        "sw sp, 52(%0)\n\t"
        :
        : "r"(env)
        : "memory"
    );
#endif
#endif

    return 0;
}

void longjmp(jmp_buf env, int val) {
    if (!val) {
        val = 1;
    }

#if defined(__x86_64__)
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
#elif defined(__i386__)
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
#elif defined(__riscv)
#if __riscv_xlen == 64
    asm volatile(
        "ld ra, 0(%0)\n\t"
        "ld s0, 8(%0)\n\t"
        "ld s1, 16(%0)\n\t"
        "ld s2, 24(%0)\n\t"
        "ld s3, 32(%0)\n\t"
        "ld s4, 40(%0)\n\t"
        "ld s5, 48(%0)\n\t"
        "ld s6, 56(%0)\n\t"
        "ld s7, 64(%0)\n\t"
        "ld s8, 72(%0)\n\t"
        "ld s9, 80(%0)\n\t"
        "ld s10, 88(%0)\n\t"
        "ld s11, 96(%0)\n\t"
        "ld sp, 104(%0)\n\t"
        "mv a0, %1\n\t"
        "jr ra\n\t"
        :
        : "r"(env), "r"(val)
        : "memory", "a0"
    );
#else
    asm volatile(
        "lw ra, 0(%0)\n\t"
        "lw s0, 4(%0)\n\t"
        "lw s1, 8(%0)\n\t"
        "lw s2, 12(%0)\n\t"
        "lw s3, 16(%0)\n\t"
        "lw s4, 20(%0)\n\t"
        "lw s5, 24(%0)\n\t"
        "lw s6, 28(%0)\n\t"
        "lw s7, 32(%0)\n\t"
        "lw s8, 36(%0)\n\t"
        "lw s9, 40(%0)\n\t"
        "lw s10, 44(%0)\n\t"
        "lw s11, 48(%0)\n\t"
        "lw sp, 52(%0)\n\t"
        "mv a0, %1\n\t"
        "jr ra\n\t"
        :
        : "r"(env), "r"(val)
        : "memory", "a0"
    );
#endif
#endif

    __builtin_unreachable();
}
