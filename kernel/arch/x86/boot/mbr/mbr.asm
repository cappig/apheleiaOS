bits 16
section .mbr

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
    sti
    mov [boot_drive], dl

    ; Check if we have int 0x13 extensions
    mov ah, 0x41
    mov bx, 0x55aa
    int 0x13
    jc no_int13
    cmp bx, 0xaa55
    jne no_int13
    test cx, 1                   ; bit 0: AH=42h/43h extended disk access
    jz no_int13

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

    ; Load in 64K-safe chunks to avoid DMA boundary crossings
    mov [save_lba], eax
    mov [save_cnt], cx

    mov word [dest_seg], 0x0000
    mov word [dest_off], 0x7c00
    mov word [chunk_max], 66

.read_chunk:
    mov cx, [save_cnt]
    mov bx, [chunk_max]
    cmp cx, bx
    jbe .cnt_ok
    mov cx, bx
.cnt_ok:
    mov si, dap
    mov [si + 2], cx
    mov ax, [dest_off]
    mov [si + 4], ax
    mov ax, [dest_seg]
    mov [si + 6], ax
    mov eax, [save_lba]
    mov [si + 8], eax

    mov ah, 0x42
    mov dl, [boot_drive]

    int 0x13
    jc read_error

    add word [save_lba], cx
    jnc .no_carry
    inc word [save_lba + 2]

.no_carry:
    sub word [save_cnt], cx
    jz .load_done

    add word [dest_seg], 0x1000
    mov word [dest_off], 0x0000
    mov word [chunk_max], 128
    jmp .read_chunk

.load_done:
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
align 4
dap:
    db 16                       ; size
    db 0                        ; reserved
    dw 0                        ; sector count
    dw 0x7c00                   ; offset
    dw 0                        ; segment
    dq 0                        ; lba

save_cnt:  dw 0
save_lba:  dd 0
boot_drive: db 0
dest_seg:  dw 0
dest_off:  dw 0
chunk_max: dw 0

msg_no_part db 'no valid partition found', 0
msg_read_error db 'disk read error', 0
msg_no_int13 db 'extended read not supported', 0

; Save space for a partition table
times 446-($-$$) db 0
