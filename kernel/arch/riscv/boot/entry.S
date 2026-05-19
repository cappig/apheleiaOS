.section .init
.option norvc

.type _start, @function
.globl _start
_start:
    .cfi_startproc

.option push
.option norelax
    la gp, __global_pointer$
.option pop

    /* Reset satp and disable interrupts for now */
    csrw satp, zero
    csrw mie, zero
    csrw mstatus, zero

    /* Setup stack */
    la sp, __stack_top

    /* Clear the BSS section */
    la t0, __bss_start
    la t1, __bss_end
1:
    bgeu t0, t1, 2f
#if __riscv_xlen == 64
    sd zero, 0(t0)
    addi t0, t0, 8
#else
    sw zero, 0(t0)
    addi t0, t0, 4
#endif
    j 1b
2:
    call boot_main

3:
    wfi
    j 3b

    .cfi_endproc
