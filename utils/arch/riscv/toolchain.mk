RISCV_FRISC ?= false

DOCKER_ARCH_VARS := \
	RISCV_FRISC \
	RISCV_UART_STRIDE \
	RISCV_UART0 \
	RISCV_KERNEL_ADDR \
	RISCV_SCRATCH_OFFSET

ifeq ($(RISCV_FRISC),true)
PROFILE ?= small
else
PROFILE ?= fast
endif

riscv_gcc = $(shell utils/arch/riscv/pick_gcc.sh $(1) $(2) $(3))

GNU_CC_CANDIDATES_riscv_64 := riscv64-unknown-elf-gcc riscv-none-elf-gcc riscv64-elf-gcc
GNU_CC_CANDIDATES_riscv_32 := riscv32-unknown-elf-gcc riscv32-elf-gcc $(GNU_CC_CANDIDATES_riscv_64)

ifndef GNU_CC_riscv_64
GNU_CC_riscv_64 := $(call riscv_gcc,rv64ima_zicsr,lp64,$(GNU_CC_CANDIDATES_riscv_64))
endif
ifndef GNU_CC_riscv_32
GNU_CC_riscv_32 := $(call riscv_gcc,rv32ima_zicsr,ilp32,$(GNU_CC_CANDIDATES_riscv_32))
endif

ifndef GNU_LD_riscv_64
GNU_LD_riscv_64 := $(call gcc_tool,$(GNU_CC_riscv_64),ld,riscv64-unknown-elf-ld riscv64-elf-ld riscv-none-elf-ld)
endif
ifndef GNU_LD_riscv_32
GNU_LD_riscv_32 := $(call gcc_tool,$(GNU_CC_riscv_32),ld,riscv32-unknown-elf-ld riscv32-elf-ld riscv64-unknown-elf-ld riscv64-elf-ld riscv-none-elf-ld)
endif

ifndef GNU_AR_riscv_64
GNU_AR_riscv_64 := $(call gcc_tool,$(GNU_CC_riscv_64),ar,riscv64-unknown-elf-ar riscv64-elf-ar riscv-none-elf-ar)
endif
ifndef GNU_AR_riscv_32
GNU_AR_riscv_32 := $(call gcc_tool,$(GNU_CC_riscv_32),ar,riscv32-unknown-elf-ar riscv32-elf-ar riscv64-unknown-elf-ar riscv64-elf-ar riscv-none-elf-ar)
endif

ifndef GNU_OC_riscv_64
GNU_OC_riscv_64 := $(call gcc_tool,$(GNU_CC_riscv_64),objcopy,riscv64-unknown-elf-objcopy riscv64-elf-objcopy riscv-none-elf-objcopy)
endif
ifndef GNU_OC_riscv_32
GNU_OC_riscv_32 := $(call gcc_tool,$(GNU_CC_riscv_32),objcopy,riscv32-unknown-elf-objcopy riscv32-elf-objcopy riscv64-unknown-elf-objcopy riscv64-elf-objcopy riscv-none-elf-objcopy)
endif

ifndef GNU_ST_riscv_64
GNU_ST_riscv_64 := $(call gcc_tool,$(GNU_CC_riscv_64),strip,riscv64-unknown-elf-strip riscv64-elf-strip riscv-none-elf-strip)
endif
ifndef GNU_ST_riscv_32
GNU_ST_riscv_32 := $(call gcc_tool,$(GNU_CC_riscv_32),strip,riscv32-unknown-elf-strip riscv32-elf-strip riscv64-unknown-elf-strip riscv64-elf-strip riscv-none-elf-strip)
endif

LLVM_AR_riscv_64 ?= $(call pick_tool,llvm-ar ar)
LLVM_CC_riscv_64 ?= clang --target=riscv64-unknown-elf
LLVM_LD_riscv_64 ?= ld.lld
LLVM_OC_riscv_64 ?= llvm-objcopy
LLVM_ST_riscv_64 ?= llvm-strip

LLVM_AR_riscv_32 ?= $(call pick_tool,llvm-ar ar)
LLVM_CC_riscv_32 ?= clang --target=riscv32-unknown-elf
LLVM_LD_riscv_32 ?= ld.lld
LLVM_OC_riscv_32 ?= llvm-objcopy
LLVM_ST_riscv_32 ?= llvm-strip

LIBGCC_FALLBACK_CC := $(GNU_CC_$(ARCH))

# Avoid Clang auto-detecting a GCC archive for the wrong RISC-V ABI.
ifeq ($(TOOLCHAIN),llvm)
RTLIB_FLAGS := --rtlib=compiler-rt
endif
