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
#endif

    __builtin_unreachable();
}
