bits 64

extern main
extern __libc_init

; Wrapper around main()
global _start
_start:
    ; Fetch the arguments from the stack
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    lea rdx, [rsp + 16 + rdi*8] ; envp

    call __libc_init

    call main

    ; Call exit() with main's return code
    mov rdi, rax
    mov rax, 0
    int 0x80

    hlt
