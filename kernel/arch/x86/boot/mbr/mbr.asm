bits 16
section .text

; Tiny MBR stub that gets placed at the beginning of the disk image
; It jumps to the second stage loader (/boot/bios.bin)
; We pass in the file inode (and the file size to save on precious space)
; and the MBR reads that inode into memory and jumps to it

global _start
_start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    cld
    cli

    ; Move the MBR to a lower address so that we can load the real bootloader at 0x7c00
    mov si, 0x7c00
    mov di, 0x500
    mov cx, 512/2
    rep movsw

    jmp 0:relocated

relocated:
    mov [drive], dl

    ; Check if we have INT 13h extensions
    mov ah, 0x41
    mov bx, 0x55aa
    int 0x13
    jc error

    ; Read the ext2 superblock
    mov word [dap_lba], 2
    mov byte [dap_sectors], 2
    mov word [dap_buf], 0x1000
    call read

    ; Verify the ext2 magic number
    cmp word [0x1000 + 56], 0xef53
    jne error

    ; Calculate block size: 1024 << log_block_size
    mov cl, [0x1000 + 24]
    mov ax, 1024
    shl ax, cl
    mov [block_size], ax

    ; Calculate sectors per block
    shr ax, 9
    mov [sectors_per_block], al

    ; Calculate block group: (inode - 1) / inodes_per_group
    mov ebx, dword [0x1000 + 40]
    mov eax, [inode_num]
    dec eax
    xor edx, edx
    div ebx                             ; EAX = group, EDX = inode index

    ; EAX = inode * inode_size
    movzx ebx, word [0x1000 + 88]        ; This assumes major ver > 1
    mov eax, edx
    mul ebx

    ; Calculate inode block: EAX / block_size
    movzx ebx, word [block_size]
    xor edx, edx
    div ebx
    mov [inode_group_index], ax
    mov [inode_group_offset], dx

    ; Calculate block group descriptor table location
    ; Block 2 if block size > 1024, else 1
    mov ax, [block_size]
    cmp ax, 1024
    mov eax, 1
    ja read_bgd
    inc eax

read_bgd:
    ; Read the BGD to 0x1000
    mov word [dap_buf], 0x1000
    call block_to_lba

    ; Get the first inode block
    mov eax, [0x1000 + 8]
    movzx ebx, word [inode_group_index]
    add eax, ebx                        ; Add inode offset

    ; Read inode block to 0x1000
    mov word [dap_buf], 0x1000
    call block_to_lba

    ; Point to inode structure
    movzx esi, word [inode_group_offset]
    add si, 0x1000

    ; Calculate file size / block count from the inode directly
    mov eax, dword [si + 4]             ; EAX = file size
    movzx ecx, word [block_size]        ; ECX = block size

    ; EAX = (file_size + block_size - 1) / block_size
    dec ecx
    add eax, ecx
    inc ecx
    xor edx, edx
    div ecx

    mov cx, ax                          ; CX = number of blocks in file

    ; SI = Direct Block Pointer #0
    add si, 40

    ; Setup loading, begin at 0x7c00
    mov di, 0x7c00
    xor bx, bx

    ; We can load the 12 direct blocks and any blocks on the first indirect block
load_direct:
    cmp bx, 12
    jae load_indirect

    cmp bx, cx
    jae boot

    lodsd                               ; [si] -> eax; si += 4

    test eax, eax
    jz next_direct

    call load_block

next_direct:
    inc bx
    jmp load_direct

load_indirect:
    cmp bx, cx
    jae boot

    hlt

    lodsd

    mov word [dap_buf], 0x1000
    call block_to_lba

    ; Process indirect entries
    mov si, 0x1000

indirect_loop:
    cmp bx, cx
    jae boot

    lodsd

    test eax, eax
    jz next_indirect

    call load_block

next_indirect:
    inc bx

    mov ax, [block_size]
    add ax, 0x1000

    cmp si, ax
    jb indirect_loop

boot:
    mov dl, [drive]
    hlt
    jmp 0:0x7c00

; Load block (eax) to di, advance di
load_block:
    push si
    push bx

    mov [dap_buf], di
    call block_to_lba

    movzx bx, word [block_size]
    add di, bx

    pop bx
    pop si
    ret

; Convert ext2 block index to disk LBA and read it
block_to_lba:
    movzx ebx, byte [sectors_per_block]
    mov [dap_sectors], bl
    mul ebx
    mov dword [dap_lba], eax
    jmp read

read:
    push di
    movzx ecx, byte [dap_sectors]
    mov di, [dap_buf]
.loop:
    mov word [dap_buf], di
    mov dl, [drive]
    mov byte [dap_sectors], 1
    mov ah, 0x42
    mov si, dap
    int 0x13
    jc error
    
    add di, 512
    inc dword [dap_lba]
    loop .loop
    pop di
    ret

error:
;     mov si, err_msg
; .l:
;     lodsb
;     test al, al
;     jz .h
;     mov ah, 0x0e
;     xor bh, bh
;     int 0x10
;     jmp .l
.h:
    hlt
    jmp .h

err_msg: db 'ERR', 0

; DAP structure
dap:
    db  0x10
    db  0x00
dap_sectors:
    dw  0
dap_buf:
    dw  0
    dw  0
dap_lba:
    dq  0

drive:              db 0
block_size:         dw 0
sectors_per_block:  db 0
inode_group_index:  dw 0
inode_group_offset: dw 0

times 432-($-$$) db 0

; This gets patched in during compile time
inode_num:          dd 0

; Save space for a partition table
times 446-($-$$) db 0
times 64 db 0

; Mark the sector as bootable
dw 0xaa55
