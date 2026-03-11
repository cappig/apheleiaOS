bits 64
section .text

%define GDT_KERNEL_CODE 0x8

global arch_context_switch
arch_context_switch:
    mov rsp, rdi

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

    cmp qword [rsp + 8*3], GDT_KERNEL_CODE
    je .no_swapgs
    swapgs
.no_swapgs:

    add rsp, 8*2
    iretq
