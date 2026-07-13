bits 32
section .text

%define ISR_COUNT 256
%define FRAME_EIP    52
%define FRAME_CS     56
%define FRAME_EFLAGS 60

%macro generate_int_stub 1
isr_stub_%+%1:
%if %1 == 8
    mov eax, dword [esp + 4]

    cmp eax, 0x08
    je .no_error%+%1

    cmp eax, 0x1b
    je .no_error%+%1

    ; double fault: CPU already pushed error code
    push dword %1
    jmp isr_common_stub
.no_error%+%1:
    ; irq0 style: no error code pushed
    push dword 0
    push dword %1

    jmp isr_common_stub
%elif (%1 == 10) || (%1 == 11) || (%1 == 12) || (%1 == 13) || (%1 == 14) \
   || (%1 == 17) || %1 == 21 || %1 == 29 || %1 == 30
    ; cpu already pushed error code, only push vector number
    push dword %1
    jmp isr_common_stub
%else
    ; for vectors without error code, push placeholder then vector
    push dword 0
    push dword %1

    jmp isr_common_stub
%endif
%endmacro

%assign i 0
%rep ISR_COUNT
    generate_int_stub i
%assign i i+1
%endrep

global isr_stub_table
isr_stub_table:
%assign i 0
%rep ISR_COUNT
    dd isr_stub_%+i
%assign i i+1
%endrep

extern isr_handler
extern arch_bad_switch_frame
extern __kernel_end

isr_common_stub:
    ; User code may enter with DF set. The iret frame preserves that value, but
    ; kernel C code must always run with the ABI-required forward direction.
    cld

    sub esp, 16
    mov word [esp + 12], ds
    mov word [esp + 8], es
    mov word [esp + 4], fs
    mov word [esp], gs
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    pop eax

    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi
    push ebp

    ; Interrupts can arrive at any alignment. EBX is already saved in the
    ; frame and is callee-saved by the i386 ABI, so use it to restore ESP.
    mov eax, esp
    mov ebx, esp
    and esp, -16
    sub esp, 12
    push eax
    call isr_handler
    mov esp, ebx

    mov eax, esp
    mov edx, [esp + FRAME_EIP]
    mov ecx, [esp + FRAME_CS]

    test dword [esp + FRAME_EFLAGS], 0x2
    jz .bad_frame
    test dword [esp + FRAME_EFLAGS], 0x27000
    jnz .bad_frame

    cmp ecx, 0x8
    je .kernel_frame
    cmp ecx, 0x1b
    jne .bad_frame

    test edx, edx
    jz .bad_frame
    cmp edx, 0xb0000000
    jb .resume_frame
    jmp .bad_frame

.kernel_frame:
    cmp edx, 0xc0000000
    jb .bad_frame
    cmp edx, __kernel_end
    jb .resume_frame
.bad_frame:
    push ecx
    push edx
    push eax
    call arch_bad_switch_frame
.resume_frame:

    mov ax, [esp + 40]
    mov ds, ax
    mov ax, [esp + 36]
    mov es, ax
    mov ax, [esp + 32]
    mov fs, ax
    mov ax, [esp + 28]
    mov gs, ax

    pop ebp
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax

    add esp, 16
    add esp, 8
    iret
