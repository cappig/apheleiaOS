.section .text.entry
.globl _start
.type _start, @function

_start:
.option push
.option norelax
    la gp, __global_pointer$
.option pop

    la sp, __stack_top
    call _kern_entry

1:
    wfi
    j 1b
