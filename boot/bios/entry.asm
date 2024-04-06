bits 16
section .entry

extern __bss_start
extern __bss_end

extern _load_entry

global _entry
_entry:
    jmp _start
    nop

; El Torito Boot Information Table
times 8-($-$$) db 0
global _boot_info
_boot_info:
.pvd_lba    dd 0
.boot_lba   dd 0
.boot_len   dd 0
.checksum   dd 0
.reserved   times 40 db 0

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

    ; Save the boot drive number
    mov [boot_drive], dx

    ; Read the drive parameters. We need to know the sector size
    mov ah, 0x48
    mov si, _drive_params
    int 0x13
    jc halt

    ; At this point only the first 4 sectors of the boot loader file are loaded
    mov eax, [_boot_info.boot_len]

    ; If the file is larger than 4*512 load the rest
    cmp eax, 512*4
    jle .load_done

.load_rest:

    ; Subtract the 4 HDD sectors that are already loaded
    sub eax, 512*4

    ; Calculate the number of sectors to load
    xor ecx, ecx
    mov cx, [_drive_params.sector_size]
    div ecx

    xor ebx, ebx
    test edx, edx
    setnz bl
    add bx, ax

    ; Calculate the lba to load
    ; The lba in _boot_info is in iso sectors (2048 bytes)
    mov eax, [_boot_info.boot_lba]

    add eax, 1

    ; If our sectors aren't 2048 bytes long assume 512 byte HDD sectors
    cmp ecx, 2048
    je .read

    mov ecx, 4
    mul ecx

.read:

    mov dword [dap.lba], eax
    mov word [dap.sectors], bx

    ; Read the rest of the bootloader from disk
    mov dx, [boot_drive]
    mov ah, 0x42
    xor di, di
    mov si, dap

    int 0x13
    jc halt

.load_done:

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
    mov dx, [boot_drive]
    push dx
    call _load_entry

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

global _drive_params
_drive_params:
.size:          dw 0x1e
.info_flags:    dw 0
.cyl_count:     dd 0
.head_count:    dd 0
.phys_sect:     dd 0
.abs_sect:      dq 0
.sector_size:   dw 0
.edd:           dd 0

; Disk address packet
dap:
.size:      dw 0x10
.sectors:   dw 0
.offset:    dd 0x8400
.lba:       dq 0

boot_drive: dw 0
