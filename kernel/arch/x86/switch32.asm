bits 32
section .text

global arch_context_switch
arch_context_switch:
    mov eax, [esp + 4]
    mov esp, eax

    pop ebp
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    pop eax

    add esp, 8
    test dword [esp + 4], 0x3
    jz .to_kernel
    push eax
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    pop eax
    iret

.to_kernel:
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    pop eax
    iret
