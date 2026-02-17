bits 16
section .entry

extern __bss_start
extern __bss_end

extern _load_entry
extern _gdt_desc

global _start
_start:
    jmp 0:main

main:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov [boot_drive], dl

    mov sp, 0x7c00

    ; Enable the A20 line via the fast method
    in al, 0x92
    or al, 2
    and al, 0xfe        ; clear bit 0 (system reset) before writing back
    out 0x92, al

    sti

    ; Clear screen by resetting the VGA mode to 80x25
    mov ah, 0x00
    mov al, 0x03
    int 0x10

    cli
    lgdt [_gdt_desc]

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

    ; Jump to the C entrypoint with the saved boot drive number
    movzx edx, byte [boot_drive]
    push edx
    call _load_entry

halt:
    hlt
    jmp halt

boot_drive: db 0
