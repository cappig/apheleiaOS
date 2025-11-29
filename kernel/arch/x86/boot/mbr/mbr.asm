bits 16
section .text

; Tiny MBR stub that gets placed at the beginning of the disk image
; It loads and jums to the second stage loader

global _start
_start:
    cli

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov sp, 0x7c00

    ; Move the MBR to a lower address so that we can load the real bootloader at 0x7c00
    mov si, sp
    mov di, 0x500
    mov cx, 512/2
    rep movsw

    jmp 0:relocated

relocated:
    ; Check if we have int 0x13 extensions
    mov ah, 0x41
    mov bx, 0x55aa
    int 0x13
    jc no_int13
    cmp bx, 0xaa55
    jc no_int13

    ; Look for the boot partition
    mov si, 0x500 + 0x1be
    mov cx, 4

next_part:
    mov al, [si]
    cmp al, 0x80                ; is it marked as bootable
    je found_part

    add si, 16

    loop next_part

    jmp no_partition

found_part:
    mov eax, [si + 8]           ; start lba
    mov ecx, [si + 12]          ; number of sectors

    mov si, dap
    mov word [si + 2], cx
    mov dword [si + 8], eax

    ; Read the bootloader to 0x7c00
    mov ah, 0x42
    int 0x13
    jc read_error

    ; Far jump to the scond stage
    jmp 0x0000:0x7c00

print:
    lodsb

    or al, al
    jz .done

    mov ah, 0x0e
    int 0x10

    jmp print
.done:
    ret

read_error:
    mov si, msg_read_error
    call print
    jmp $

no_int13:
    mov si, msg_no_int13
    call print
    jmp $

no_partition:
    mov si, msg_no_part
    call print
    jmp $


; Disk address packet
dap:
    db 16                       ; size
    db 0                        ; reserved
    dw 0                        ; sector count
    dw 0x7c00                   ; offset
    dw 0                        ; segment
    dq 0                        ; lba

msg_no_part db 'no valid partition found', 0
msg_read_error db 'disk read error', 0
msg_no_int13 db 'extended read not supported', 0

; Save space for a partition table
times 446-($-$$) db 0
