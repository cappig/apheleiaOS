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
    cli
    cld

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov [boot_drive], dl

    mov sp, 0x7c00

    ; enable the A20 line and verify it really opened
    call enable_a20
    jc halt

    sti

    ; clear screen by resetting the VGA mode to 80x25
    mov ah, 0x00
    mov al, 0x03
    int 0x10

    cli
    lgdt [_gdt_desc]

    ; set the protected mode bit
    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp 0x18:protected_mode

enable_a20:
    in al, 0x92
    or al, 2
    and al, 0xfe        ; clear bit 0 (system reset) before writing back
    out 0x92, al

    call check_a20
    jnc .done

    mov ax, 0x2401
    int 0x15
    call check_a20

.done:
    ret

check_a20:
    push ax
    push ds
    push es
    push si
    push di

    xor ax, ax
    mov ds, ax
    mov ax, 0xffff
    mov es, ax

    mov si, 0x0500
    mov di, 0x0510

    mov ax, [ds:si]
    push ax
    mov ax, [es:di]
    push ax

    mov word [ds:si], 0x55aa
    mov word [es:di], 0xaa55
    cmp word [ds:si], 0x55aa

    pop ax
    mov [es:di], ax
    pop ax
    mov [ds:si], ax

    pop di
    pop si
    pop es
    pop ds
    pop ax

    jne .closed
    clc
    ret

.closed:
    stc
    ret

bits 32
protected_mode:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; zero out the .BSS
    xor al, al
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    rep stosb

    ; jump to the C entrypoint with the saved boot drive number
    movzx edx, byte [boot_drive]
    push edx
    call _load_entry

halt:
    hlt
    jmp halt

boot_drive: db 0
