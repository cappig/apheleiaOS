bits 32
section .text

global arch_context_switch
extern arch_context_switch_bad_frame
extern __kernel_end
arch_context_switch:
    mov eax, [esp + 4]
    mov esp, eax

    cmp dword [esp + 56], 0x8
    jne .restore_segments
    mov edx, [esp + 52]
    cmp edx, 0xc0000000
    jb .bad_frame
    cmp edx, __kernel_end
    jb .restore_segments
.bad_frame:
    mov ecx, [esp + 56]
    push ecx
    push edx
    push eax
    call arch_context_switch_bad_frame

.restore_segments:

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
