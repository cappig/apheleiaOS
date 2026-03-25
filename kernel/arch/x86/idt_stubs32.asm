bits 32
section .text

%define ISR_COUNT 256

%macro generate_int_stub 1
isr_stub_%+%1:
%if %1 == 8
    mov eax, dword [esp + 4]

    cmp eax, 0x08
    je .no_error%+%1

    cmp eax, 0x1b
    je .no_error%+%1

    ; Double fault: CPU already pushed error code.
    push dword %1
    jmp isr_common_stub
.no_error%+%1:
    ; IRQ0 style: no error code pushed.
    push dword 0
    push dword %1

    jmp isr_common_stub
%elif (%1 == 10) || (%1 == 11) || (%1 == 12) || (%1 == 13) || (%1 == 14) \
   || (%1 == 17) || %1 == 21 || %1 == 29 || %1 == 30
    ; CPU already pushed error code, only push vector number
    push dword %1
    jmp isr_common_stub
%else
    ; For vectors without error code, push placeholder then vector
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
extern arch_context_switch_bad_frame
extern __kernel_end

isr_common_stub:
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

    mov eax, esp
    push eax
    call isr_handler
    add esp, 4

    mov eax, esp
    cmp dword [esp + 56], 0x8
    jne .resume_frame
    mov edx, [esp + 52]
    cmp edx, 0xc0000000
    jb .bad_frame
    cmp edx, __kernel_end
    jb .resume_frame
.bad_frame:
    mov ecx, [esp + 56]
    push ecx
    push edx
    push eax
    call arch_context_switch_bad_frame
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
