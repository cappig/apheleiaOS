bits 32
section .text

%define FRAME_EIP    52
%define FRAME_CS     56
%define FRAME_EFLAGS 60

global arch_context_switch
extern arch_bad_switch_frame
extern __kernel_end
arch_context_switch:
    mov eax, [esp + 4]
    mov esp, eax

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
    jb .restore_segments
    jmp .bad_frame

.kernel_frame:
    cmp edx, 0xc0000000
    jb .bad_frame
    cmp edx, __kernel_end
    jb .restore_segments
.bad_frame:
    push ecx
    push edx
    push eax
    call arch_bad_switch_frame

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
