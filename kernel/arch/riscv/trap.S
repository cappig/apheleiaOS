.section .text
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

#define SSTATUS_SPP (1 << 8)
#define SSTATUS_SUM (1 << 18)

#define OFF_RA      (0 * REGBYTES)
#define OFF_GP      (1 * REGBYTES)
#define OFF_TP      (2 * REGBYTES)
#define OFF_T0      (3 * REGBYTES)
#define OFF_T1      (4 * REGBYTES)
#define OFF_T2      (5 * REGBYTES)
#define OFF_S0      (6 * REGBYTES)
#define OFF_S1      (7 * REGBYTES)
#define OFF_A0      (8 * REGBYTES)
#define OFF_A1      (9 * REGBYTES)
#define OFF_A2      (10 * REGBYTES)
#define OFF_A3      (11 * REGBYTES)
#define OFF_A4      (12 * REGBYTES)
#define OFF_A5      (13 * REGBYTES)
#define OFF_A6      (14 * REGBYTES)
#define OFF_A7      (15 * REGBYTES)
#define OFF_S2      (16 * REGBYTES)
#define OFF_S3      (17 * REGBYTES)
#define OFF_S4      (18 * REGBYTES)
#define OFF_S5      (19 * REGBYTES)
#define OFF_S6      (20 * REGBYTES)
#define OFF_S7      (21 * REGBYTES)
#define OFF_S8      (22 * REGBYTES)
#define OFF_S9      (23 * REGBYTES)
#define OFF_S10     (24 * REGBYTES)
#define OFF_S11     (25 * REGBYTES)
#define OFF_T3      (26 * REGBYTES)
#define OFF_T4      (27 * REGBYTES)
#define OFF_T5      (28 * REGBYTES)
#define OFF_T6      (29 * REGBYTES)
#define OFF_SEPC    (30 * REGBYTES)
#define OFF_SSTATUS (31 * REGBYTES)
#define OFF_SP      (32 * REGBYTES)
#define OFF_SCAUSE  (33 * REGBYTES)
#define OFF_STVAL   (34 * REGBYTES)

#define FRAME_WORDS 35
#define FRAME_SIZE  (FRAME_WORDS * REGBYTES)

#define SCRATCH_T1  (-2 * REGBYTES)
#define SCRATCH_T2  (-3 * REGBYTES)
#define USER_FRAME_BIAS (FRAME_SIZE + (3 * REGBYTES))

.globl riscv_trap_entry
.balign 4
.type riscv_trap_entry, @function
riscv_trap_entry:
    csrrw t0, sscratch, t0
    STORE t1, SCRATCH_T1(t0)
    STORE t2, SCRATCH_T2(t0)

    csrr t1, sstatus
    andi t1, t1, SSTATUS_SPP
    bnez t1, 1f

    mv t2, sp
    addi sp, t0, -USER_FRAME_BIAS
    j 2f

1:
    mv t2, sp
    addi sp, sp, -FRAME_SIZE

2:
    STORE ra, OFF_RA(sp)
    STORE gp, OFF_GP(sp)
    STORE tp, OFF_TP(sp)

    csrr t1, sscratch
    STORE t1, OFF_T0(sp)
    LOAD t1, SCRATCH_T1(t0)
    STORE t1, OFF_T1(sp)
    LOAD t1, SCRATCH_T2(t0)
    STORE t1, OFF_T2(sp)

    STORE s0, OFF_S0(sp)
    STORE s1, OFF_S1(sp)
    STORE a0, OFF_A0(sp)
    STORE a1, OFF_A1(sp)
    STORE a2, OFF_A2(sp)
    STORE a3, OFF_A3(sp)
    STORE a4, OFF_A4(sp)
    STORE a5, OFF_A5(sp)
    STORE a6, OFF_A6(sp)
    STORE a7, OFF_A7(sp)
    STORE s2, OFF_S2(sp)
    STORE s3, OFF_S3(sp)
    STORE s4, OFF_S4(sp)
    STORE s5, OFF_S5(sp)
    STORE s6, OFF_S6(sp)
    STORE s7, OFF_S7(sp)
    STORE s8, OFF_S8(sp)
    STORE s9, OFF_S9(sp)
    STORE s10, OFF_S10(sp)
    STORE s11, OFF_S11(sp)
    STORE t3, OFF_T3(sp)
    STORE t4, OFF_T4(sp)
    STORE t5, OFF_T5(sp)
    STORE t6, OFF_T6(sp)

    csrr t1, sepc
    STORE t1, OFF_SEPC(sp)
    csrr t1, sstatus
    STORE t1, OFF_SSTATUS(sp)
    STORE t2, OFF_SP(sp)
    csrr t1, scause
    STORE t1, OFF_SCAUSE(sp)
    csrr t1, stval
    STORE t1, OFF_STVAL(sp)

    la t1, riscv_cpu_local_ptr
    LOAD t1, 0(t1)
    mv tp, t1

    la t1, riscv_kernel_sp
    LOAD t1, 0(t1)
    csrw sscratch, t1

    li t1, SSTATUS_SUM
    csrs sstatus, t1

    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    mv a0, sp
    call riscv_handle_trap
    j 3f

.globl riscv_resched_trap
.balign 4
.type riscv_resched_trap, @function
riscv_resched_trap:
    ebreak
    ret

.globl arch_context_switch
.balign 4
.type arch_context_switch, @function
arch_context_switch:
    mv sp, a0

3:
    mv t6, sp

    LOAD t0, OFF_SEPC(t6)
    csrw sepc, t0
    LOAD t0, OFF_SSTATUS(t6)
    csrw sstatus, t0

    la t0, riscv_kernel_sp
    LOAD t0, 0(t0)
    csrw sscratch, t0

    LOAD ra, OFF_RA(t6)
    LOAD gp, OFF_GP(t6)
    LOAD tp, OFF_TP(t6)
    LOAD t0, OFF_T0(t6)
    LOAD t1, OFF_T1(t6)
    LOAD t2, OFF_T2(t6)
    LOAD s0, OFF_S0(t6)
    LOAD s1, OFF_S1(t6)
    LOAD a0, OFF_A0(t6)
    LOAD a1, OFF_A1(t6)
    LOAD a2, OFF_A2(t6)
    LOAD a3, OFF_A3(t6)
    LOAD a4, OFF_A4(t6)
    LOAD a5, OFF_A5(t6)
    LOAD a6, OFF_A6(t6)
    LOAD a7, OFF_A7(t6)
    LOAD s2, OFF_S2(t6)
    LOAD s3, OFF_S3(t6)
    LOAD s4, OFF_S4(t6)
    LOAD s5, OFF_S5(t6)
    LOAD s6, OFF_S6(t6)
    LOAD s7, OFF_S7(t6)
    LOAD s8, OFF_S8(t6)
    LOAD s9, OFF_S9(t6)
    LOAD s10, OFF_S10(t6)
    LOAD s11, OFF_S11(t6)
    LOAD t3, OFF_T3(t6)
    LOAD t4, OFF_T4(t6)
    LOAD t5, OFF_T5(t6)
    LOAD sp, OFF_SP(t6)
    LOAD t6, OFF_T6(t6)
    sret
