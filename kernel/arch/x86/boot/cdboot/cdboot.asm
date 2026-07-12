bits 16
org 0

%define BOOT_SEGMENT 0x07c0
%define RELOC_SEGMENT 0x0600
%define LOAD_SEGMENT 0x07c0
%define SECTOR_PARAS 0x0080

start:
    jmp BOOT_SEGMENT:loaded

loaded:
    cli
    cld

    mov ax, BOOT_SEGMENT
    mov ds, ax
    mov ax, RELOC_SEGMENT
    mov es, ax

    xor si, si
    xor di, di
    mov cx, 2048 / 2
    rep movsw

    jmp RELOC_SEGMENT:relocated

relocated:
    cli

    mov ax, RELOC_SEGMENT
    mov ds, ax

    xor ax, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    sti
    mov [boot_drive], dl

    mov ah, 0x41
    mov bx, 0x55aa
    int 0x13
    jc no_extensions
    cmp bx, 0xaa55
    jne no_extensions
    test cx, 1
    jz no_extensions

    mov eax, [bios_lba]
    mov [dap_lba], eax
    cmp word [bios_blocks], 0
    jz invalid_image

    mov word [dap_segment], LOAD_SEGMENT

.load:
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc read_error

    inc dword [dap_lba]
    add word [dap_segment], SECTOR_PARAS

    dec word [bios_blocks]
    jnz .load

    mov dl, [boot_drive]
    jmp 0:0x7c00

print:
    lodsb
    test al, al
    jz .done

    mov ah, 0x0e
    int 0x10
    jmp print

.done:
    ret

no_extensions:
    mov si, msg_no_extensions
    jmp fatal

invalid_image:
    mov si, msg_invalid_image
    jmp fatal

read_error:
    mov si, msg_read_error

fatal:
    call print

.halt:
    cli
    hlt
    jmp .halt

align 4
dap:
    db 16
    db 0
    dw 1
    dw 0
dap_segment:
    dw LOAD_SEGMENT
dap_lba:
    dq 0

boot_drive: db 0

msg_no_extensions db 'extended disk reads unavailable', 0
msg_invalid_image db 'invalid CD boot image', 0
msg_read_error db 'CD boot read error', 0

align 4
boot_metadata:
    db 'APHELEIA_CD_BOOT'
bios_lba:
    dd 0
bios_blocks:
    dw 0

times 2048-($-$$) db 0
