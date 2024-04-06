bits 16
section .text

; Tiny MBR stub that gets placed at the beginning of the disk image
; It jums to the second stage loader (boot/bios/...)
; This code is based on the grub2 MBR bootloader

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

    ; We expect the higher half to be zero. Bail if it's not
    ; This is what grub 2 does ;)
    mov eax, dword [boot_file_sector + 4]
    or eax, eax
    jnz boot_field_err

    ; Xorrisofs adds an offset of 4
    mov eax, dword [boot_file_sector]
    sub eax, 4

    ; Load the info into the dap and load the first 4 sectors
    mov word [dap.lba], ax

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

boot_field_err:
   mov si, field_err_msg
   call print_string
   jmp halt

field_err_msg   db 'Malformed boot file position field!', 13, 10, 0
disk_err_msg    db 'Error reading from disk!', 13, 10, 0
halt_err_msg    db 'Halted execution!', 13, 10, 0

; Disk address packet
dap:
.size:      dw 0x10
.sectors:   dw 4
.offset:    dd 0x7c00
.lba:       dq 0

; GRUB2 uses this offset to store the lba of the boot file
; Xorrisofs can patch this in during build time so we use it here as well
boot_file_sector equ $$+0x1b0


; Mark as bootable
times 510-($-$$) db 0
dw 0xaa55
