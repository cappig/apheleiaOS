.section .text
.globl _start
_start:
    ; https://wiki.osdev.org/RISC-V_Bare_Bones
    la sp, _boot_stack_top
1:
    wfi
    j 1b

.section .bss
.align 12
_boot_stack:
    .space 4096
_boot_stack_top:
