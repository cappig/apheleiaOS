.section .bss
.option norvc

#if __riscv_xlen == 64
#define REGBYTES 8
#define STORE sd
#define LOAD ld
#else
#define REGBYTES 4
#define STORE sw
#define LOAD lw
#endif

#define MCAUSE_S_ECALL 9
#define MCAUSE_M_TIMER 7
#define MIP_STIP_BIT   (1 << 5)

#define RISCV_MCALL_EXT_TIMER       0x4154494d
#define RISCV_MCALL_TIMER_ARM_DELTA 0
#define RISCV_MCALL_TIMER_READ_TIME 1

#define SCR_T1 (0 * REGBYTES)
#define SCR_T2 (1 * REGBYTES)
#define SCR_T3 (2 * REGBYTES)
#define SCR_T4 (3 * REGBYTES)
#define SCR_T5 (4 * REGBYTES)
#define SCR_T6 (5 * REGBYTES)

.balign 16
.globl riscv_boot_mtrap_scratch
riscv_boot_mtrap_scratch:
    .space 6 * REGBYTES

.section .text
.option norvc

.globl mtrap_entry
.type mtrap_entry, @function
mtrap_entry:
    csrrw t0, mscratch, t0
    STORE t1, SCR_T1(t0)
    STORE t2, SCR_T2(t0)
    STORE t3, SCR_T3(t0)
    STORE t4, SCR_T4(t0)
    STORE t5, SCR_T5(t0)
    STORE t6, SCR_T6(t0)

    csrr t1, mcause
    bltz t1, .Linterrupt

    li t2, MCAUSE_S_ECALL
    beq t1, t2, .Lecall
    j .Lhalt

.Linterrupt:
    slli t2, t1, 1
    srli t2, t2, 1
    li t3, MCAUSE_M_TIMER
    beq t2, t3, .Ltimer
    j .Lhalt

.Ltimer:
    la t1, riscv_boot_timer_cmp_addr
    LOAD t1, 0(t1)
    beqz t1, .Lhalt
#if __riscv_xlen == 64
    li t2, -1
    sd t2, 0(t1)
#else
    li t2, -1
    sw t2, 4(t1)
    sw t2, 0(t1)
#endif
    li t2, MIP_STIP_BIT
    csrs mip, t2
    j .Lreturn

.Lecall:
    li t2, RISCV_MCALL_EXT_TIMER
    bne a7, t2, .Lecall_fail
    li t2, RISCV_MCALL_TIMER_ARM_DELTA
    bne a6, t2, .Lecall_other

    la t1, riscv_boot_timer_time_addr
    LOAD t1, 0(t1)
    la t2, riscv_boot_timer_cmp_addr
    LOAD t2, 0(t2)
    beqz t1, .Lecall_fail
    beqz t2, .Lecall_fail

#if __riscv_xlen == 64
    ld t3, 0(t1)
    add t3, t3, a0

    li t4, MIP_STIP_BIT
    csrc mip, t4
    sd t3, 0(t2)
#else
.Lread_mtime:
    lw t3, 4(t1)
    lw t4, 0(t1)
    lw t5, 4(t1)
    bne t3, t5, .Lread_mtime

    add t4, t4, a0
    sltu t5, t4, a0
    add t3, t3, a1
    add t3, t3, t5

    li t5, MIP_STIP_BIT
    csrc mip, t5

    li t5, -1
    sw t5, 4(t2)
    sw t4, 0(t2)
    sw t3, 4(t2)
#endif

    csrr t1, mepc
    addi t1, t1, 4
    csrw mepc, t1
    li a0, 0
    li a1, 0
    j .Lreturn

.Lecall_other:
    li t2, RISCV_MCALL_TIMER_READ_TIME
    bne a6, t2, .Lecall_fail

    la t1, riscv_boot_timer_time_addr
    LOAD t1, 0(t1)
    beqz t1, .Lecall_fail

#if __riscv_xlen == 64
    ld t3, 0(t1)
    csrr t1, mepc
    addi t1, t1, 4
    csrw mepc, t1
    li a0, 0
    mv a1, t3
    li a2, 0
#else
.Lread_mtime_value:
    lw t3, 4(t1)
    lw t4, 0(t1)
    lw t5, 4(t1)
    bne t3, t5, .Lread_mtime_value
    csrr t1, mepc
    addi t1, t1, 4
    csrw mepc, t1
    li a0, 0
    mv a1, t4
    mv a2, t3
#endif
    j .Lreturn

.Lecall_fail:
    csrr t1, mepc
    addi t1, t1, 4
    csrw mepc, t1
    li a0, -1
    li a1, 0
    j .Lreturn

.Lhalt:
1:
    wfi
    j 1b

.Lreturn:
    LOAD t1, SCR_T1(t0)
    LOAD t2, SCR_T2(t0)
    LOAD t3, SCR_T3(t0)
    LOAD t4, SCR_T4(t0)
    LOAD t5, SCR_T5(t0)
    LOAD t6, SCR_T6(t0)
    csrrw t0, mscratch, t0
    mret
