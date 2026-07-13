bits 16
section .text

global smp_trampoline64_start
global smp_trampoline64_end
global smp_trampoline64_cr0
global smp_trampoline64_cr4
global smp_trampoline64_cr3
global smp_trampoline64_efer
global smp_trampoline64_entry
global smp_trampoline64_arg
global smp_trampoline64_stack

%define OFF(sym) (sym - smp_trampoline64_start)

smp_trampoline64_start:
    cli

    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x0ff0

    xor eax, eax
    mov ax, cs
    shl eax, 4
    mov dword [OFF(tramp64_base)], eax

    mov edx, eax
    mov bx, OFF(tramp64_gdt) + 8
    call tramp64_patch_desc

    mov edx, eax
    mov bx, OFF(tramp64_gdt) + 16
    call tramp64_patch_desc

    mov edx, eax
    add edx, OFF(tramp64_gdt)
    mov dword [OFF(tramp64_gdt_desc) + 2], edx

    lgdt [OFF(tramp64_gdt_desc)]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:OFF(tramp64_pm32)

tramp64_patch_desc:
    mov word [bx + 2], dx
    shr edx, 16
    mov byte [bx + 4], dl
    mov byte [bx + 7], dh
    ret

bits 32
tramp64_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov ebx, dword [OFF(tramp64_base)]

    mov eax, dword [OFF(smp_trampoline64_cr4)]
    mov cr4, eax

    mov ecx, 0xC0000080
    mov eax, dword [OFF(smp_trampoline64_efer)]
    xor edx, edx
    wrmsr

    mov eax, dword [OFF(smp_trampoline64_cr3)]
    mov cr3, eax

    mov eax, dword [OFF(smp_trampoline64_cr0)]
    mov cr0, eax

    lea eax, [ebx + OFF(tramp64_lm64)]
    push dword 0x18
    push eax
    retf

bits 64
tramp64_lm64:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, qword [rbx + OFF(smp_trampoline64_stack)]
    and rsp, -16
    push qword 0

    mov rdi, qword [rbx + OFF(smp_trampoline64_arg)]
    mov rax, qword [rbx + OFF(smp_trampoline64_entry)]
    jmp rax

tramp64_halt:
    cli
    hlt
    jmp tramp64_halt

align 8
tramp64_gdt_desc:
    dw tramp64_gdt_end - tramp64_gdt - 1
    dd 0

tramp64_gdt:
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

    ; 0x18: long mode code segment (base ignored in long mode)
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0x9a
    db 0xaf
    db 0x00

    ; 0x20: flat data segment
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0x92
    db 0xcf
    db 0x00
tramp64_gdt_end:

align 8
tramp64_base:
    dd 0
smp_trampoline64_cr0:
    dd 0
smp_trampoline64_cr4:
    dd 0
smp_trampoline64_cr3:
    dd 0
smp_trampoline64_efer:
    dd 0
smp_trampoline64_entry:
    dq 0
smp_trampoline64_arg:
    dq 0
smp_trampoline64_stack:
    dq 0

smp_trampoline64_end:
