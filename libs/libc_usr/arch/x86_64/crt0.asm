bits 64
section .text

global _start
extern main

_start:
    xor rbp, rbp
    call main
    mov rdi, rax
    mov rax, 0
    int 0x80

halt:
    hlt
    jmp halt
