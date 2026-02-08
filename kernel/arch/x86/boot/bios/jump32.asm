bits 32
section .text

; void jump_to_kernel_32(u32 entry, u32 boot_info, u32 stack);
global jump_to_kernel_32
jump_to_kernel_32:
    mov ebx, dword [esp+4]
    mov eax, dword [esp+8]
    mov esp, dword [esp+12]

    jmp ebx

halt:
    hlt
    jmp halt
