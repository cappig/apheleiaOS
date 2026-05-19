.section .text
.globl _start
.type _start, @function

.extern environ
.extern main

_start:
.option push
.option norelax
    la gp, __global_pointer$
.option pop

    mv fp, zero
    lw a0, 0(sp)
    addi a1, sp, 4
    slli t0, a0, 2
    add t0, a1, t0
    addi a2, t0, 4

    la t1, environ
    sw a2, 0(t1)

    call main

    li a7, 0
    ecall

1:
    wfi
    j 1b
