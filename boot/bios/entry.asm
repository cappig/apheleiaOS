bits 16
section .entry

extern __bss_start
extern __bss_end

extern _load_entry

global _start
_start:
    cli
    cld

    jmp 0:main
main:

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov sp, 0x7c00

    ; Clear screen by resetting the VGA mode to 80x25
    mov ah, 0x00
    mov al, 0x03
    int 0x10

    ; Enable the A20 line via the fast method
    in al, 0x92
    test al, 2
    jnz .a20_enabled

    or al, 2
    and al, 0xfe
    out 0x92, al
.a20_enabled:

    lgdt [gdt_desc]

    ; Set the protected mode bit
    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp 0x18:protected_mode
bits 32
protected_mode:

    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Zero out the .BSS
    xor al, al
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    rep stosb

    ; Jump to the C entrypoint
    push dx
    ;call _load_entry
    mov eax, 0x6969420

halt:
    hlt
    jmp halt


; Global descriptor table
global gdt_desc
gdt_desc:
    dw gdt.end-gdt - 1
    dd gdt

gdt:
    ; Null segment
    dq 0

    ; 16 bit code = 0x08
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0b10011011
    db 0b00001111
    db 0x00

    ; 16 bit data = 0x10
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0b10010011
    db 0b00001111
    db 0x00

    ; 32 bit code = 0x18
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0b10011011
    db 0b11001111
    db 0x00

    ; 32 bit data = 0x20
    dw 0xffff
    dw 0x0000
    db 0x00
    db 0b10010011
    db 0b11001111
    db 0x00
.end:
