bits 16
section .text

; Tiny MBR stub that gets placed at the beginning of the disk image
; It jums to the second stage loader (boot/bios/...)
; This code is based on the syslinux MBR bootloader

global _start
_start:
    cli
    cld

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov sp, 0x7c00

    ; Move the MBR to a lower address so that we can load the real bootloader at 0x7c00
    mov si, sp
    mov di, 0x600
    mov cx, 512/4
    rep movsd

jmp 0:main
main:

    ; Scan the partition table for a valid active partition
    mov si, partition_table
    xor ax, ax

    mov cx, 4
table_loop:
    test byte [si], 0x80 ; is this an active partition
    jz .skip
    inc ax
    mov di, si
.skip:
    add si, 16
    loop table_loop

    ; Assume that our table is fucked if we have more than one active partition
    cmp ax, 1
    jnz part_table_err

    ; Load the partiton info into the dap
    mov ax, [di + 8]
    mov [dap.lba], ax
    mov ax, [di + 12]
    mov [dap.sectors], ax

    ; Read the bootloader from disk
    mov ah, 0x42
    mov si, dap
    int 0x13
    jc disk_err

    ; Jump to bios/entry.asm:_start
    jmp 0:0x7c00

halt:
    mov si, halt_err_msg
    call print_string
.loop:
    hlt
    jmp .loop


print_string:
    lodsb

    or al, al
    jz .done

    mov ah, 0x0e
    int 0x10

    jmp print_string
.done:
    ret

disk_err:
   mov si, disk_err_msg
   call print_string
   jmp halt

part_table_err:
   mov si, part_table_err_msg
   call print_string
   jmp halt


disk_err_msg        db 'Error reading from disk!', 13, 10, 0
part_table_err_msg  db 'Malformed partition table!', 13, 10, 0
halt_err_msg        db 'Halted execution!', 13, 10, 0

; Disk address packet
dap:
.size:      dw 0x10
.sectors:   dw 0
.offset:    dd 0x7c00
.lba:       dq 0

partition_table equ $$+446
partition_table_size equ 16


; Mark as bootable
times 510-($-$$) db 0
dw 0xaa55
