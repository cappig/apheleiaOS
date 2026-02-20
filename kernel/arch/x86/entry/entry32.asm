bits 32
section .bootstrap

extern _kern_entry

global _start
_start:
    cli
    xor ebp, ebp

    ; boot info pointer is in eax, but the 32 bit calling convention 
    ; requires the args to be passed on the stack
    push eax
    call _kern_entry

halt:
    hlt
    jmp halt
