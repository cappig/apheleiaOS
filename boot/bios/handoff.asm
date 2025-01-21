bits 32
section .text
; Enter long mode and jump to the kernel entry point

; void jump_to_kernel(u64 entry, u64 boot_handoff, u64 stack);
global jump_to_kernel
jump_to_kernel:
    lgdt [gdt_desc_64]

    jmp 0x08:.long_mode

bits 64
.long_mode:

    cli
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rbx, qword [esp+4]
    mov rax, qword [esp+12]
    mov rsp, qword [esp+20]

    jmp rbx

halt:
    hlt
    jmp halt


section .data
gdt_desc_64:
.size: dw gdt_64.end - gdt_64 - 1
.addr: dd gdt_64

gdt_64:
.null:
    dq 0x00
.longmode_code:
    dw 0x0000
    dw 0x0000
    db 0x00
    db 0x9a
    db 0x20
    db 0x00
.longmode_data:
    dw 0x0000
    dw 0x0000
    db 0x00
    db 0x92
    db 0x00
    db 0x00
.end:
