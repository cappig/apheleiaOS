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

#define RA_OFFSET      (0 * REGBYTES)
#define GP_OFFSET      (1 * REGBYTES)
#define TP_OFFSET      (2 * REGBYTES)
#define T0_OFFSET      (3 * REGBYTES)
#define T1_OFFSET      (4 * REGBYTES)
#define T2_OFFSET      (5 * REGBYTES)
#define S0_OFFSET      (6 * REGBYTES)
#define S1_OFFSET      (7 * REGBYTES)
#define A0_OFFSET      (8 * REGBYTES)
#define A1_OFFSET      (9 * REGBYTES)
#define A2_OFFSET      (10 * REGBYTES)
#define A3_OFFSET      (11 * REGBYTES)
#define A4_OFFSET      (12 * REGBYTES)
#define A5_OFFSET      (13 * REGBYTES)
#define A6_OFFSET      (14 * REGBYTES)
#define A7_OFFSET      (15 * REGBYTES)
#define S2_OFFSET      (16 * REGBYTES)
#define S3_OFFSET      (17 * REGBYTES)
#define S4_OFFSET      (18 * REGBYTES)
#define S5_OFFSET      (19 * REGBYTES)
#define S6_OFFSET      (20 * REGBYTES)
#define S7_OFFSET      (21 * REGBYTES)
#define S8_OFFSET      (22 * REGBYTES)
#define S9_OFFSET      (23 * REGBYTES)
#define S10_OFFSET     (24 * REGBYTES)
#define S11_OFFSET     (25 * REGBYTES)
#define T3_OFFSET      (26 * REGBYTES)
#define T4_OFFSET      (27 * REGBYTES)
#define T5_OFFSET      (28 * REGBYTES)
#define T6_OFFSET      (29 * REGBYTES)
#define SEPC_OFFSET    (30 * REGBYTES)
#define SSTATUS_OFFSET (31 * REGBYTES)
#define SP_OFFSET      (32 * REGBYTES)
#define SCAUSE_OFFSET  (33 * REGBYTES)
#define STVAL_OFFSET   (34 * REGBYTES)

#define FRAME_WORDS 35
#define FRAME_SIZE  (FRAME_WORDS * REGBYTES)

#define SCRATCH_T1  (-2 * REGBYTES)
#define SCRATCH_T2  (-3 * REGBYTES)
#define USER_FRAME_BIAS (FRAME_SIZE + (3 * REGBYTES))

.globl trap_entry
.balign 4
.type trap_entry, @function
trap_entry:
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
    STORE ra, RA_OFFSET(sp)
    STORE gp, GP_OFFSET(sp)

    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    STORE tp, TP_OFFSET(sp)

    csrr t1, sscratch
    STORE t1, T0_OFFSET(sp)
    LOAD t1, SCRATCH_T1(t0)
    STORE t1, T1_OFFSET(sp)
    LOAD t1, SCRATCH_T2(t0)
    STORE t1, T2_OFFSET(sp)

    STORE s0, S0_OFFSET(sp)
    STORE s1, S1_OFFSET(sp)
    STORE a0, A0_OFFSET(sp)
    STORE a1, A1_OFFSET(sp)
    STORE a2, A2_OFFSET(sp)
    STORE a3, A3_OFFSET(sp)
    STORE a4, A4_OFFSET(sp)
    STORE a5, A5_OFFSET(sp)
    STORE a6, A6_OFFSET(sp)
    STORE a7, A7_OFFSET(sp)
    STORE s2, S2_OFFSET(sp)
    STORE s3, S3_OFFSET(sp)
    STORE s4, S4_OFFSET(sp)
    STORE s5, S5_OFFSET(sp)
    STORE s6, S6_OFFSET(sp)
    STORE s7, S7_OFFSET(sp)
    STORE s8, S8_OFFSET(sp)
    STORE s9, S9_OFFSET(sp)
    STORE s10, S10_OFFSET(sp)
    STORE s11, S11_OFFSET(sp)
    STORE t3, T3_OFFSET(sp)
    STORE t4, T4_OFFSET(sp)
    STORE t5, T5_OFFSET(sp)
    STORE t6, T6_OFFSET(sp)

    csrr t1, sepc
    STORE t1, SEPC_OFFSET(sp)
    csrr t1, sstatus
    STORE t1, SSTATUS_OFFSET(sp)
    STORE t2, SP_OFFSET(sp)
    csrr t1, scause
    STORE t1, SCAUSE_OFFSET(sp)
    csrr t1, stval
    STORE t1, STVAL_OFFSET(sp)

    la t1, cpu_local_ptr
    LOAD t1, 0(t1)
    mv tp, t1

    la t1, kernel_sp
    LOAD t1, 0(t1)
    csrw sscratch, t1

    li t1, SSTATUS_SUM
    csrs sstatus, t1

    mv a0, sp
    call trap_handle
    j 3f

.globl trap_resched
.balign 4
.type trap_resched, @function
trap_resched:
    ebreak
    ret

.globl arch_context_switch
.balign 4
.type arch_context_switch, @function
arch_context_switch:
    mv sp, a0

3:
    mv a0, sp
    call riscv_trap_return_prepare
    mv t6, sp

    LOAD t0, SEPC_OFFSET(t6)
    csrw sepc, t0
    LOAD t0, SSTATUS_OFFSET(t6)
    csrw sstatus, t0

    la t0, kernel_sp
    LOAD t0, 0(t0)
    csrw sscratch, t0

    LOAD ra, RA_OFFSET(t6)
    LOAD gp, GP_OFFSET(t6)
    LOAD tp, TP_OFFSET(t6)
    LOAD t0, T0_OFFSET(t6)
    LOAD t1, T1_OFFSET(t6)
    LOAD t2, T2_OFFSET(t6)
    LOAD s0, S0_OFFSET(t6)
    LOAD s1, S1_OFFSET(t6)
    LOAD a0, A0_OFFSET(t6)
    LOAD a1, A1_OFFSET(t6)
    LOAD a2, A2_OFFSET(t6)
    LOAD a3, A3_OFFSET(t6)
    LOAD a4, A4_OFFSET(t6)
    LOAD a5, A5_OFFSET(t6)
    LOAD a6, A6_OFFSET(t6)
    LOAD a7, A7_OFFSET(t6)
    LOAD s2, S2_OFFSET(t6)
    LOAD s3, S3_OFFSET(t6)
    LOAD s4, S4_OFFSET(t6)
    LOAD s5, S5_OFFSET(t6)
    LOAD s6, S6_OFFSET(t6)
    LOAD s7, S7_OFFSET(t6)
    LOAD s8, S8_OFFSET(t6)
    LOAD s9, S9_OFFSET(t6)
    LOAD s10, S10_OFFSET(t6)
    LOAD s11, S11_OFFSET(t6)
    LOAD t3, T3_OFFSET(t6)
    LOAD t4, T4_OFFSET(t6)
    LOAD t5, T5_OFFSET(t6)
    LOAD sp, SP_OFFSET(t6)
    LOAD t6, T6_OFFSET(t6)
    sret
