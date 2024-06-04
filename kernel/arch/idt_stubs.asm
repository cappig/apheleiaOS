bits 64
section .text

extern isr_handler

%define ISR_COUNT 256

; Generate functions that push the appropriate intrrupt number and jump to the handler
; This allows us to identify the intrrupt later on
%macro generate_int_stub 1
isr_stub_%+%1:
%if !(%1 == 8 || (%1 > 9 && %1 < 15) || %1 == 17 || %1 == 30)
    ; For the interrupts that don't push a error code we push 0 to
    ; the stack so that it doesn't get missaligned
    push qword 0
%endif
    push qword %1
    jmp isr_common_stub
%endmacro

%assign i 0
%rep ISR_COUNT
    generate_int_stub i
%assign i i+1
%endrep

; Construct a table of function pointers
; We index this later to find the correct handler
global isr_stub_table
isr_stub_table:
%assign i 0
%rep ISR_COUNT
    dq isr_stub_%+i
%assign i i+1
%endrep


global isr_common_stub
isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, ds
    push ax
    mov ax, es
    push ax
    mov ax, fs
    push ax
    mov ax, gs
    push ax

    mov rdi, rsp
    call isr_handler

    pop ax
    mov gs, ax
    pop ax
    mov fs, ax
    pop ax
    mov es, ax
    pop ax
    mov ds, ax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Pop error code and interrupt number
    add rsp, 8*2

    iretq
