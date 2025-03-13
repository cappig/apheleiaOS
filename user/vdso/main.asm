bits 64

; The VDSO (virtual dynamic shared object) is a small shared library that gets mapped
; to a processes virtual memory space. It may provide common kernel subroutines
; We use it here to provide a trampoline for signal returns

global signal_trampoline
signal_trampoline:
    ; Call sigreturn
    mov rax, 10
    int 0x80
    int3
