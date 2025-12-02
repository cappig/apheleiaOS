bits 64
section .bootstrap

extern _kern_entry

global _start
_start:
    cli
    mov rdi, rax
    xor rbp, rbp
    ; call _kern_entry
    mov rax, 0x6969696

halt:
    hlt
    jmp halt

; FIXME: JUST TO MAKE THIS STUB BUILD! REMOVE ASAP ASAP ASAP!!!!!!!
global _external_alloc
_external_alloc:
    dq 0
