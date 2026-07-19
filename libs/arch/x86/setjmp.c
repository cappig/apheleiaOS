#define APHELEIA_SETJMP_NO_MACRO

#include <setjmp.h>

// clang-format off
#if defined(__x86_64__)
__asm__(
    ".globl setjmp\n"
    "setjmp:\n"
    "movq %rbx, 0(%rdi)\n"
    "movq %rbp, 8(%rdi)\n"
    "movq %r12, 16(%rdi)\n"
    "movq %r13, 24(%rdi)\n"
    "movq %r14, 32(%rdi)\n"
    "movq %r15, 40(%rdi)\n"
    "movq (%rsp), %rax\n"
    "movq %rax, 48(%rdi)\n"
    "leaq 8(%rsp), %rax\n"
    "movq %rax, 56(%rdi)\n"
    "xorl %eax, %eax\n"
    "ret\n"
    "\n"
    ".globl longjmp\n"
    "longjmp:\n"
    "movq %rdi, %rdx\n"
    "movl %esi, %eax\n"
    "testl %eax, %eax\n"
    "jnz 0f\n"
    "movl $1, %eax\n"
    "0:\n"
    "movq 0(%rdx), %rbx\n"
    "movq 8(%rdx), %rbp\n"
    "movq 16(%rdx), %r12\n"
    "movq 24(%rdx), %r13\n"
    "movq 32(%rdx), %r14\n"
    "movq 40(%rdx), %r15\n"
    "movq 56(%rdx), %rsp\n"
    "jmp *48(%rdx)\n"
);
#elif defined(__i386__)
__asm__(
    ".globl setjmp\n"
    "setjmp:\n"
    "movl 4(%esp), %edx\n"
    "movl %ebx, 0(%edx)\n"
    "movl %esi, 4(%edx)\n"
    "movl %edi, 8(%edx)\n"
    "movl %ebp, 12(%edx)\n"
    "movl (%esp), %eax\n"
    "movl %eax, 16(%edx)\n"
    "leal 4(%esp), %eax\n"
    "movl %eax, 20(%edx)\n"
    "xorl %eax, %eax\n"
    "ret\n"
    "\n"
    ".globl longjmp\n"
    "longjmp:\n"
    "movl 4(%esp), %edx\n"
    "movl 8(%esp), %eax\n"
    "testl %eax, %eax\n"
    "jnz 0f\n"
    "movl $1, %eax\n"
    "0:\n"
    "movl 0(%edx), %ebx\n"
    "movl 4(%edx), %esi\n"
    "movl 8(%edx), %edi\n"
    "movl 12(%edx), %ebp\n"
    "movl 20(%edx), %esp\n"
    "jmp *16(%edx)\n"
);
#else
#error "Unsupported x86 target"
#endif
// clang-format on
