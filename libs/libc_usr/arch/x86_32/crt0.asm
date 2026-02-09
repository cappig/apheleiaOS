bits 32
section .text

global _start
extern main

_start:
    xor ebp, ebp
    call main
    mov ebx, eax
    mov eax, 0
    int 0x80

halt:
    hlt
    jmp halt
