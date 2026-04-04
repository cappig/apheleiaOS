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

#define OFF_RA       (0 * REGBYTES)
#define OFF_GP       (1 * REGBYTES)
#define OFF_TP       (2 * REGBYTES)
#define OFF_T0       (3 * REGBYTES)
#define OFF_T1       (4 * REGBYTES)
#define OFF_T2       (5 * REGBYTES)
#define OFF_S0       (6 * REGBYTES)
#define OFF_S1       (7 * REGBYTES)
#define OFF_A0       (8 * REGBYTES)
#define OFF_A1       (9 * REGBYTES)
#define OFF_A2       (10 * REGBYTES)
#define OFF_A3       (11 * REGBYTES)
#define OFF_A4       (12 * REGBYTES)
#define OFF_A5       (13 * REGBYTES)
#define OFF_A6       (14 * REGBYTES)
#define OFF_A7       (15 * REGBYTES)
#define OFF_S2       (16 * REGBYTES)
#define OFF_S3       (17 * REGBYTES)
#define OFF_S4       (18 * REGBYTES)
#define OFF_S5       (19 * REGBYTES)
#define OFF_S6       (20 * REGBYTES)
#define OFF_S7       (21 * REGBYTES)
#define OFF_S8       (22 * REGBYTES)
#define OFF_S9       (23 * REGBYTES)
#define OFF_S10      (24 * REGBYTES)
#define OFF_S11      (25 * REGBYTES)
#define OFF_T3       (26 * REGBYTES)
#define OFF_T4       (27 * REGBYTES)
#define OFF_T5       (28 * REGBYTES)
#define OFF_T6       (29 * REGBYTES)
#define OFF_MEPC     (30 * REGBYTES)
#define OFF_MSTATUS  (31 * REGBYTES)
#define OFF_SP       (32 * REGBYTES)
#define OFF_MCAUSE   (33 * REGBYTES)
#define OFF_MTVAL    (34 * REGBYTES)
#define OFF_MHARTID  (35 * REGBYTES)

#define FRAME_WORDS  36
#define FRAME_SIZE   (FRAME_WORDS * REGBYTES)
#define SCRATCH_T1   (-2 * REGBYTES)
#define SCRATCH_T2   (-3 * REGBYTES)
#define FRAME_BIAS   (FRAME_SIZE + (3 * REGBYTES))

.globl riscv_boot_mtrap_entry
.type riscv_boot_mtrap_entry, @function
riscv_boot_mtrap_entry:
    csrrw t0, mscratch, t0
    STORE t1, SCRATCH_T1(t0)
    STORE t2, SCRATCH_T2(t0)

    mv t2, sp
    addi sp, t0, -FRAME_BIAS

    STORE ra, OFF_RA(sp)
    STORE gp, OFF_GP(sp)
    STORE tp, OFF_TP(sp)

    csrr t1, mscratch
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

    csrr t1, mepc
    STORE t1, OFF_MEPC(sp)
    csrr t1, mstatus
    STORE t1, OFF_MSTATUS(sp)
    STORE t2, OFF_SP(sp)
    csrr t1, mcause
    STORE t1, OFF_MCAUSE(sp)
    csrr t1, mtval
    STORE t1, OFF_MTVAL(sp)
    csrr t1, mhartid
    STORE t1, OFF_MHARTID(sp)

    mv a0, sp
    call riscv_boot_handle_trap

    mv t6, sp

    LOAD t0, OFF_MEPC(t6)
    csrw mepc, t0
    LOAD t0, OFF_MSTATUS(t6)
    csrw mstatus, t0
    addi t0, t6, FRAME_BIAS
    csrw mscratch, t0

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
    mret
