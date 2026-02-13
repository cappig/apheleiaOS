bits 64
section .text

global _start
extern main
extern environ

_start:
    xor rbp, rbp
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    lea rdx, [rsi + rdi * 8 + 8]
    mov [rel environ], rdx
    call main
    mov rdi, rax
    mov rax, 0
    int 0x80

halt:
    hlt
    jmp halt
