bits 64
section .bootstrap

extern _kern_entry

global _start
_start:
    cli
    mov rdi, rax
    xor rbp, rbp
    call _kern_entry

halt:
    hlt
    jmp halt
