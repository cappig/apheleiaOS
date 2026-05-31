bits 32
section .text

global _start
extern exit
extern main
extern environ

_start:
    xor ebp, ebp
    mov eax, [esp]
    lea edx, [esp + 4]
    lea ecx, [edx + eax * 4 + 4]
    mov [environ], ecx
    and esp, 0xfffffff0
    sub esp, 8
    push edx
    push eax
    call main
    push eax
    call exit

halt:
    hlt
    jmp halt
