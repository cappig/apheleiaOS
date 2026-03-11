bits 32
section .text

global _start
extern main
extern environ

_start:
    xor ebp, ebp
    mov eax, [esp]
    lea edx, [esp + 4]
    lea ecx, [edx + eax * 4 + 4]
    mov [environ], ecx
    push edx
    push eax
    call main
    mov ebx, eax
    mov eax, 0
    int 0x80

halt:
    hlt
    jmp halt
