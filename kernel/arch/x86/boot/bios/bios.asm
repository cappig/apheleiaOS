bits 32
section .text

; temporarily drop back to real mode to call a BIOS interrupt
; based on the Linux implementation (arch/x86/boot/bioscall.S)

%define REGS_B 44                           ; size of the x_regs struct
%define SREGS_B 16                          ; size of non scratch regs

extern _gdt_desc

; void bios_call(u8 number, regs* in_regs, regs* out_regs)
global bios_call
bios_call:
    ; self modify the code
    mov al, byte [esp+4]
    mov byte [.n], al

    ; save non scratch regs
    push ebx
    push esi
    push edi
    push ebp

    ;; jump to real mode
    cli
    jmp 0x08:.real_entry

bits 16
.real_entry:

    ; unset protected mode bit
    mov eax, cr0
    and al, ~1
    mov cr0, eax

    ; actually enter real mode
    jmp 0x00:.real_start
.real_start:

    ; set up real mode segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; enable interrupts
    sti

    ; move in_regs to the stack
    mov esi, dword [esp+SREGS_B+8]          ; address of in_regs

    sub esp, REGS_B                         ; make space for the registers
    mov edi, esp                            ; destination address

    mov ecx, REGS_B/4                       ; repeat cx times
    cld
    rep movsd

    ; pop the in_regs from the stack
    popad
    popfd
    pop gs
    pop fs
    pop es
    pop ds

    db 0xcd                                 ; int opcode
.n: db 0                                    ; int number is written here

    ; save return registers
    push ds
    push es
    push fs
    push gs
    pushfd
    pushad

    ;; return to protected mode
    cli
    lgdt [_gdt_desc]                        ; load 32 bit GDT

    ; set protected mode bit
    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp 0x18:.protected_mode

bits 32
.protected_mode:

    ; set up protected mode segments
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; move out_regs from stack
    mov esi, esp                            ; start address

    mov edi, dword [esp+REGS_B+SREGS_B+12]  ; destination address

    mov ecx, REGS_B/4                       ; repeat cx times
    cld
    rep movsd

    add esp, REGS_B                         ; move the stack back

    ; restore non scratch regs
    pop ebp
    pop edi
    pop esi
    pop ebx

    ret
