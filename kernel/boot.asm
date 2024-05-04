bits 64
section .bootstrap

extern _kern_entry

global _start
_start:
    mov rdi, rax
    call _kern_entry

halt:
    hlt
    jmp halt
