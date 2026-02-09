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
    iret
