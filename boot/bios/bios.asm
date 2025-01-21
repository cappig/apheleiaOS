bits 32
section .text

; Temporarily drop back to real mode to call a BIOS interrupt
; Based on the Linux implementation (arch/x86/boot/bioscall.S)

%define REGS_B 44                           ; Size of the x_regs struct
%define SREGS_B 16                          ; Size of non scratch regs

extern _gdt_desc

; void bios_call(u8 number, regs* in_regs, regs* out_regs);
global bios_call
bios_call:
    ; Self modify the code
    mov al, byte [esp+4]
    mov byte [.n], al

    ; Save non scratch regs
    push ebx
    push esi
    push edi
    push ebp

    ;; Jump to real mode
    cli
    jmp 0x08:.real_entry

bits 16
.real_entry:

    ; Unset protected mode bit
    mov eax, cr0
    and al, ~1
    mov cr0, eax

    ; Actually enter real mode
    jmp 0x00:.real_start
.real_start:

    ; Set up real mode segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Enable interrupts
    sti

    ; Move in_regs to the stack
    mov esi, dword [esp+SREGS_B+8]          ; Address of in_regs

    sub esp, REGS_B                         ; Make space for the registers
    mov edi, esp                            ; Destination address

    mov ecx, REGS_B/4                       ; repeat cx times
    rep movsd

    ; *Pop* the in_regs form the stack
    popad
    popfd
    pop gs
    pop fs
    pop es
    pop ds

    db 0xcd                                 ; int opcode
.n: db 0                                    ; int number is written here

    ; Save return registers
    push ds
    push es
    push fs
    push gs
    pushfd
    pushad

    ;; Return to protected mode
    cli
    lgdt [_gdt_desc]                        ; Load 32 bit GDT

    ; Set protected mode bit
    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp 0x18:.protected_mode

bits 32
.protected_mode:

    ; Set up protected mode segments
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Move out_regs from stack
    mov esi, esp                            ; Start address

    mov edi, dword [esp+REGS_B+SREGS_B+12]  ; Destination address

    mov ecx, REGS_B/4                       ; repeat cx times
    rep movsd

    add esp, REGS_B                         ; Move the stack back

    ; Restore non scratch regs
    pop ebp
    pop edi
    pop esi
    pop ebx

    ret
