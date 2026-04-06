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
    ld a0, 0(sp)
    addi a1, sp, 8
    slli t0, a0, 3
    add t0, a1, t0
    addi a2, t0, 8

    la t1, environ
    sd a2, 0(t1)

    call main

    li a7, 0
    ecall

1:
    wfi
    j 1b
