bits 64
section .text

%define ISR_COUNT 256
%define GDT_KERNEL_CODE 0x8

%macro generate_int_stub 1
isr_stub_%+%1:
%if (%1 == 8) || (%1 == 10) || (%1 == 11) || (%1 == 12) || (%1 == 13) \
   || (%1 == 14) || (%1 == 17) || %1 == 21 || %1 == 29 || %1 == 30
    ; CPU will already have pushed the hardware error code for these vectors,
    ; our stub should not push another. We still push the vector number.
    push qword %1
%else
    ; For vectors that don't push an error code, we push a 0 as placeholder
    push qword 0
    push qword %1
%endif
    jmp isr_common_stub
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
    dq isr_stub_%+i
%assign i i+1
%endrep

; swapgs must only be called when going from kernel mode to user mode
%macro swapgs_if_necessary 0
    cmp qword [rsp + 8*3], GDT_KERNEL_CODE
    je %%no_swapgs
    swapgs
%%no_swapgs:
%endmacro

extern isr_handler

; global isr_common_stub
isr_common_stub:
    swapgs_if_necessary

    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    mov     rdi, rsp
    call    isr_handler

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax

    swapgs_if_necessary

    ; Pop the error code and interrupt number
    add rsp, 8*2

    iretq
