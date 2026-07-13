bits 16
section .text

global smp_trampoline32_start
global smp_trampoline32_end
global smp_trampoline32_cr0
global smp_trampoline32_cr4
global smp_trampoline32_cr3
global smp_trampoline32_entry
global smp_trampoline32_arg
global smp_trampoline32_stack

%define OFF(sym) (sym - smp_trampoline32_start)

smp_trampoline32_start:
    cli

    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x0ff0

    xor eax, eax
    mov ax, cs
    shl eax, 4
    mov dword [OFF(tramp32_base)], eax

    mov edx, eax
    mov bx, OFF(tramp32_gdt) + 8
    call tramp32_patch_desc

    mov edx, eax
    mov bx, OFF(tramp32_gdt) + 16
    call tramp32_patch_desc

    mov edx, eax
    add edx, OFF(tramp32_gdt)
    mov dword [OFF(tramp32_gdt_desc) + 2], edx

    lgdt [OFF(tramp32_gdt_desc)]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:OFF(tramp32_pm32)

tramp32_patch_desc:
    mov word [bx + 2], dx
    shr edx, 16
    mov byte [bx + 4], dl
    mov byte [bx + 7], dh
    ret

bits 32
tramp32_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov ebx, dword [OFF(tramp32_base)]

    mov eax, dword [OFF(smp_trampoline32_cr3)]
    mov cr3, eax

    mov eax, dword [OFF(smp_trampoline32_cr4)]
    mov cr4, eax

    mov eax, dword [OFF(smp_trampoline32_cr0)]
    mov cr0, eax

    lea eax, [ebx + OFF(tramp32_flat32)]
    push dword 0x18
    push eax
    retf

tramp32_flat32:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov esp, dword [ebx + OFF(smp_trampoline32_stack)]
    and esp, 0xfffffff0

    push dword [ebx + OFF(smp_trampoline32_arg)]
    mov eax, dword [ebx + OFF(smp_trampoline32_entry)]
    call eax
    add esp, 4

tramp32_halt:
    cli
    hlt
    jmp tramp32_halt

align 8
tramp32_gdt_desc:
    dw tramp32_gdt_end - tramp32_gdt - 1
    dd 0

tramp32_gdt:
    dq 0x0000000000000000

    ; 0x08: boot trampoline 32-bit code segment, base patched at runtime
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0x9a
    db 0xcf
    db 0x00

    ; 0x10: boot trampoline 32-bit data segment, base patched at runtime
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0x92
    db 0xcf
    db 0x00

    ; 0x18: flat 32-bit code segment
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0x9a
    db 0xcf
    db 0x00

    ; 0x20: flat 32-bit data segment
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0x92
    db 0xcf
    db 0x00
tramp32_gdt_end:

align 8
tramp32_base:
    dd 0
smp_trampoline32_cr0:
    dd 0
smp_trampoline32_cr4:
    dd 0
smp_trampoline32_cr3:
    dd 0
smp_trampoline32_entry:
    dd 0
smp_trampoline32_arg:
    dd 0
smp_trampoline32_stack:
    dd 0

smp_trampoline32_end:
