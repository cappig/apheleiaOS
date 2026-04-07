AS := nasm
OC := objcopy
ST := strip

GNU_CC_x86_64   ?= x86_64-linux-gnu-gcc
GNU_LD_x86_64   ?= ld
GNU_OC_x86_64   ?= objcopy
GNU_ST_x86_64   ?= strip

GNU_CC_x86_32   ?= i686-elf-gcc
GNU_LD_x86_32   ?= i686-elf-ld
GNU_OC_x86_32   ?= i686-elf-objcopy
GNU_ST_x86_32   ?= i686-elf-strip

GNU_CC_riscv_64 ?= riscv64-elf-gcc
GNU_LD_riscv_64 ?= riscv64-elf-ld
GNU_OC_riscv_64 ?= riscv64-elf-objcopy
GNU_ST_riscv_64 ?= riscv64-elf-strip

GNU_CC_riscv_32 ?= riscv64-elf-gcc
GNU_LD_riscv_32 ?= riscv64-elf-ld
GNU_OC_riscv_32 ?= riscv64-elf-objcopy
GNU_ST_riscv_32 ?= riscv64-elf-strip

LLVM_CC_x86_64   ?= clang
LLVM_LD_x86_64   ?= ld.lld
LLVM_OC_x86_64   ?= llvm-objcopy
LLVM_ST_x86_64   ?= llvm-strip

LLVM_CC_x86_32   ?= clang
LLVM_LD_x86_32   ?= ld.lld
LLVM_OC_x86_32   ?= llvm-objcopy
LLVM_ST_x86_32   ?= llvm-strip

LLVM_CC_riscv_64 ?= clang --target=riscv64-unknown-elf
LLVM_LD_riscv_64 ?= ld.lld
LLVM_OC_riscv_64 ?= llvm-objcopy
LLVM_ST_riscv_64 ?= llvm-strip

LLVM_CC_riscv_32 ?= clang --target=riscv32-unknown-elf
LLVM_LD_riscv_32 ?= ld.lld
LLVM_OC_riscv_32 ?= llvm-objcopy
LLVM_ST_riscv_32 ?= llvm-strip


ifeq ($(TOOLCHAIN), gnu)
	CC := $(GNU_CC_$(ARCH))
	LD := $(GNU_LD_$(ARCH))
	OC := $(GNU_OC_$(ARCH))
	ST := $(GNU_ST_$(ARCH))
else ifeq ($(TOOLCHAIN), llvm)
	CC := $(LLVM_CC_$(ARCH))
	LD := $(LLVM_LD_$(ARCH))
	OC := $(LLVM_OC_$(ARCH))
	ST := $(LLVM_ST_$(ARCH))
else
$(error Unsupported TOOLCHAIN '$(TOOLCHAIN)')
endif

ifeq ($(strip $(CC)),)
$(error Unsupported ARCH '$(ARCH)' for TOOLCHAIN '$(TOOLCHAIN)')
endif

define cc
	@$(CC) $(CC_BASE) $(strip $(1)) -c -o $(strip $(2)) $(strip $(3))
	@printf "%s  %s\n" "CC" "$(strip $(3))"
endef

define as
	@$(AS) $(AS_BASE) $(strip $(1)) -o $(strip $(2)) $(strip $(3))
	@printf "%s  %s\n" "AS" "$(strip $(3))"
endef

define ld
	@$(LD) $(LD_BASE) $(strip $(1)) -o $(strip $(2)) $(strip $(3))
	@printf "%s  %s\n" "LD" "$(strip $(2))"
endef

define oc
	@$(OC) $(strip $(1)) $(strip $(2)) $(strip $(3))
	@printf "%s  %s\n" "OC" "$(strip $(2))"
endef

define st
	@$(ST) $(strip $(1))
	@printf "%s  %s\n" "ST" "$(strip $(1))"
endef


LIBGCC = $(shell lib=$$($(CC) $(CC_BASE) $(1) -print-libgcc-file-name 2>/dev/null); \
	if [ -f "$$lib" ]; then echo "$$lib"; fi)

LIBC_DIRS := libs/libc libs/libc_ext

CC_BASE_COMMON := \
	-Wno-unused-parameter \
	-Wno-missing-braces \
	-DVERSION=\"$(VERSION)\" \
	-DBUILD_DATE=\"$(BUILD_DATE)\" \
	-DGIT_COMMIT=\"$(GIT_COMMIT_SHORT)\"

CC_DEBUG := \
	-DDISK_DEBUG \
	-DINPUT_DEBUG \
	-DLOCK_DEBUG \
	-DMMU_DEBUG \
	-g

CC_DEBUG_EXTRA := \
	-DKMALLOC_DEBUG \
	-DSCHED_DEBUG \
	-DINT_DEBUG \
	-DSYSCALL_DEBUG

ARCH_TREE    := $(word 1, $(subst _, ,$(ARCH)))
ARCH_VARIANT := $(word 2, $(subst _, ,$(ARCH)))

include kernel/arch/$(ARCH_TREE)/build/build.mk


GCC_ANALYZER ?= false

CC_BASE_ANALYZER :=
ifeq ($(TOOLCHAIN), gnu)
ifeq ($(GCC_ANALYZER), true)
CC_BASE_ANALYZER := \
	-fanalyzer \
	-fanalyzer-transitivity
endif
endif


STRIP_KERNEL ?= true

CC_BASE_PROFILE :=
ifeq ($(PROFILE), debug)
	CC_BASE_PROFILE := \
		-Og \
		$(CC_DEBUG)
	TRACEABLE_KERNEL = true
else ifeq ($(PROFILE), debug_extra)
	CC_BASE_PROFILE := \
		-Og \
		$(CC_DEBUG) \
		$(CC_DEBUG_EXTRA)
else ifeq ($(PROFILE), small)
	CC_BASE_PROFILE := \
		-Os
else ifeq ($(PROFILE), normal)
	CC_BASE_PROFILE := \
		-O2
else ifeq ($(PROFILE), fast)
	CC_BASE_PROFILE := \
		-O3
endif

CC_BASE_TRACE :=
ifeq ($(TRACEABLE_KERNEL), true)
CC_BASE_TRACE := \
	-g \
	-fno-omit-frame-pointer
endif

CC_BASE := \
	$(CC_BASE) \
	$(CC_BASE_COMMON) \
	$(CC_BASE_ANALYZER) \
	$(CC_BASE_TRACE) \
	$(CC_BASE_PROFILE)
