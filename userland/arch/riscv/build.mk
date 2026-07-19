USER_CRT_SRC   := libs/libc_usr/arch/riscv_$(ARCH_VARIANT)/crt0.S
USER_CRTI_SRC  := libs/libc_usr/arch/riscv_$(ARCH_VARIANT)/crti.S
USER_CRTN_SRC  := libs/libc_usr/arch/riscv_$(ARCH_VARIANT)/crtn.S
USER_LD_SCRIPT := userland/arch/riscv/linker$(ARCH_VARIANT).ld

USER_AS                :=
USER_ARCH_DEFAULT_SKIP := $(USER_WM_ONLY)

USER_RISCV_64_FLAGS := -march=rv64ima_zicsr_zifencei -mabi=lp64
USER_RISCV_32_FLAGS := -march=rv32ima_zicsr_zifencei -mabi=ilp32

ifeq ($(RISCV_FRISC),true)
# Avoid the FRISC core's hardware multiply and divide unit for stability.
USER_RISCV_32_FLAGS := -march=rv32ia_zicsr_zifencei -mabi=ilp32
ROOTFS_EXTRA_BYTES ?= $(if $(filter tcc,$(USER_EXTRA_NAMES)),8388608,0)
endif

ifeq ($(ARCH_VARIANT),64)
USER_ARCH_CFLAGS := $(USER_RISCV_64_FLAGS) -mcmodel=medlow
USER_LD_EMU      := -melf64lriscv
TCC_TARGET_NAME    := riscv64
TCC_TARGET_DEFS    := -DTCC_TARGET_RISCV64
TCC_TARGET_TRIPLET := riscv64-apheleia
else ifeq ($(ARCH_VARIANT),32)
USER_ARCH_CFLAGS := $(USER_RISCV_32_FLAGS) -mcmodel=medlow
USER_LD_EMU      := -melf32lriscv
TCC_TARGET_NAME    := riscv32
TCC_TARGET_DEFS    := -DTCC_TARGET_RISCV32 -DTCC_RISCV_ilp32
TCC_TARGET_TRIPLET := riscv32-apheleia
else
$(error Unsupported RISC-V variant '$(ARCH_VARIANT)')
endif
