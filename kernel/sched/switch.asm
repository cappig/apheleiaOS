bits 64
section .text

; We use this function to bootstrap the scheduler:
; When we run the first process the rsp still has the old (bootloader allocated) kernel stack
; We have to switch to the processes kernel stack first

; void context_switch(u64 kstack, u32 cr3);
global context_switch
context_switch:
    ; By this point CR3 should already be set

    ; Switch to the process kernel stack
    ; 'kstack' must point to the process state
    mov rsp, rdi

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

    ; At this point the top of the stack contains the interrupt frame
    iretq
