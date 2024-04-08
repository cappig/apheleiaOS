bits 64

section .bootstrap

extern _k_main

global _start
_start:
    mov rax, 0xdeadbeef

    ;call _k_main

halt:
    hlt
    jmp halt
