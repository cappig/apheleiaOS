bits 32
section .bootstrap

extern _kern_entry

global _start
_start:
    cli
    xor ebp, ebp

    ; boot info pointer is provided in eax by the bootloader
    push eax
    call _kern_entry

halt:
    hlt
    jmp halt
