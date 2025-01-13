bits 64

extern main
extern __libc_init

; Wrapper around main()
global _start
_start:
    xor rbp, rbp
    xor rax, rax

    call main

    ; Call exit() with main's return code
    mov rdi, rax
    mov rax, 0
    int 0x80

    hlt
