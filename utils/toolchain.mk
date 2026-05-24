AS := nasm
OC := objcopy
ST := strip

pick_tool = $(shell utils/pick_tool.sh $(1))
gcc_tool  = $(shell utils/gcc_tool.sh "$(strip $(1))" $(2) $(3))
riscv_gcc = $(shell utils/pick_riscv_gcc.sh $(1) $(2) $(3))

GNU_CC_CANDIDATES_x86_64   := x86_64-elf-gcc x86_64-linux-gnu-gcc gcc
GNU_CC_CANDIDATES_x86_32   := i686-elf-gcc i386-elf-gcc i686-linux-gnu-gcc i386-linux-gnu-gcc
GNU_CC_CANDIDATES_riscv_64 := riscv64-unknown-elf-gcc riscv-none-elf-gcc riscv64-elf-gcc
GNU_CC_CANDIDATES_riscv_32 := $(GNU_CC_CANDIDATES_riscv_64)

# Per-arch, per-toolchain tool overrides. Defaults are resolved from common
# bare-metal and distro names; callers can still pass GNU_CC_x86_32=/path/gcc.
ifndef GNU_CC_x86_64
GNU_CC_x86_64 := $(call pick_tool,$(GNU_CC_CANDIDATES_x86_64))
endif
ifndef GNU_CC_x86_32
GNU_CC_x86_32 := $(call pick_tool,$(GNU_CC_CANDIDATES_x86_32))
endif
ifndef GNU_CC_riscv_64
GNU_CC_riscv_64 := $(call riscv_gcc,rv64ima_zicsr,lp64,$(GNU_CC_CANDIDATES_riscv_64))
endif
ifndef GNU_CC_riscv_32
GNU_CC_riscv_32 := $(call riscv_gcc,rv32ima_zicsr,ilp32,$(GNU_CC_CANDIDATES_riscv_32))
endif

ifndef GNU_LD_x86_64
GNU_LD_x86_64 := $(call gcc_tool,$(GNU_CC_x86_64),ld,x86_64-elf-ld x86_64-linux-gnu-ld ld)
endif
ifndef GNU_LD_x86_32
GNU_LD_x86_32 := $(call gcc_tool,$(GNU_CC_x86_32),ld,i686-elf-ld i386-elf-ld i686-linux-gnu-ld i386-linux-gnu-ld)
endif
ifndef GNU_LD_riscv_64
GNU_LD_riscv_64 := $(call gcc_tool,$(GNU_CC_riscv_64),ld,riscv64-unknown-elf-ld riscv64-elf-ld riscv-none-elf-ld)
endif
ifndef GNU_LD_riscv_32
GNU_LD_riscv_32 := $(call gcc_tool,$(GNU_CC_riscv_32),ld,riscv64-unknown-elf-ld riscv64-elf-ld riscv-none-elf-ld)
endif

ifndef GNU_OC_x86_64
GNU_OC_x86_64 := $(call gcc_tool,$(GNU_CC_x86_64),objcopy,x86_64-elf-objcopy x86_64-linux-gnu-objcopy objcopy)
endif
ifndef GNU_OC_x86_32
GNU_OC_x86_32 := $(call gcc_tool,$(GNU_CC_x86_32),objcopy,i686-elf-objcopy i386-elf-objcopy i686-linux-gnu-objcopy i386-linux-gnu-objcopy)
endif
ifndef GNU_OC_riscv_64
GNU_OC_riscv_64 := $(call gcc_tool,$(GNU_CC_riscv_64),objcopy,riscv64-unknown-elf-objcopy riscv64-elf-objcopy riscv-none-elf-objcopy)
endif
ifndef GNU_OC_riscv_32
GNU_OC_riscv_32 := $(call gcc_tool,$(GNU_CC_riscv_32),objcopy,riscv64-unknown-elf-objcopy riscv64-elf-objcopy riscv-none-elf-objcopy)
endif

ifndef GNU_ST_x86_64
GNU_ST_x86_64 := $(call gcc_tool,$(GNU_CC_x86_64),strip,x86_64-elf-strip x86_64-linux-gnu-strip strip)
endif
ifndef GNU_ST_x86_32
GNU_ST_x86_32 := $(call gcc_tool,$(GNU_CC_x86_32),strip,i686-elf-strip i386-elf-strip i686-linux-gnu-strip i386-linux-gnu-strip)
endif
ifndef GNU_ST_riscv_64
GNU_ST_riscv_64 := $(call gcc_tool,$(GNU_CC_riscv_64),strip,riscv64-unknown-elf-strip riscv64-elf-strip riscv-none-elf-strip)
endif
ifndef GNU_ST_riscv_32
GNU_ST_riscv_32 := $(call gcc_tool,$(GNU_CC_riscv_32),strip,riscv64-unknown-elf-strip riscv64-elf-strip riscv-none-elf-strip)
endif

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

ifneq ($(ARCH),)

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
ifeq ($(strip $(LD)),)
$(error Missing linker for ARCH '$(ARCH)' and TOOLCHAIN '$(TOOLCHAIN)')
endif
ifeq ($(strip $(OC)),)
$(error Missing objcopy for ARCH '$(ARCH)' and TOOLCHAIN '$(TOOLCHAIN)')
endif
ifeq ($(strip $(ST)),)
$(error Missing strip for ARCH '$(ARCH)' and TOOLCHAIN '$(TOOLCHAIN)')
endif

endif

define log
	@printf "%-3s  %s\n" "$(strip $(1))" "$(strip $(2))"
endef

# Rebuild objects when command-line flags change. Make tracks source
# dependencies, but it does not know when the recipe text has changed.
.PHONY: FORCE
FORCE:

define flag_stamp
$(1): FORCE
	@utils/write_flag_stamp.sh "$$@" '$(strip $($(2)))'
endef

# Compiler / assembler / linker wrappers used by arch build.mk files.
define cc
	@$(CC) $(CC_BASE) $(strip $(1)) -c -o $(strip $(2)) $(strip $(3))
	$(call log, CC, $(3))
endef

define as
	@$(AS) $(AS_BASE) $(strip $(1)) -o $(strip $(2)) $(strip $(3))
	$(call log, AS, $(3))
endef

define ld
	@$(LD) $(LD_BASE) $(strip $(1)) -o $(strip $(2)) $(strip $(3))
	$(call log, LD, $(2))
endef

define oc
	@$(OC) $(strip $(1)) $(strip $(2)) $(strip $(3))
	$(call log, OC, $(2))
endef

define st
	@$(ST) $(strip $(1))
	$(call log, ST, $(1))
endef

LIBC_DIRS := libs/libc libs/libc_ext

# Split ARCH into tree (x86, riscv) and variant (64, 32).
ARCH_TREE    := $(word 1, $(subst _, ,$(ARCH)))
ARCH_VARIANT := $(word 2, $(subst _, ,$(ARCH)))

LIBGCC_FALLBACK_CC :=
ifeq ($(ARCH_TREE), riscv)
LIBGCC_FALLBACK_CC := $(GNU_CC_$(ARCH)) $(GNU_CC_CANDIDATES_$(ARCH))
endif

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

ifeq ($(PROFILE), debug)
CC_BASE_PROFILE := -Og $(CC_DEBUG)
TRACEABLE_KERNEL = true
else ifeq ($(PROFILE), debug_extra)
CC_BASE_PROFILE := -Og $(CC_DEBUG) $(CC_DEBUG_EXTRA)
else ifeq ($(PROFILE), small)
CC_BASE_PROFILE := -Os
else ifeq ($(PROFILE), normal)
CC_BASE_PROFILE := -O2
else ifeq ($(PROFILE), fast)
CC_BASE_PROFILE := -O3
else
$(error Unsupported PROFILE '$(PROFILE)')
endif


GCC_ANALYZER ?= false
CC_BASE_ANALYZER :=
ifeq ($(TOOLCHAIN), gnu)
ifeq ($(GCC_ANALYZER), true)
CC_BASE_ANALYZER := -fanalyzer -fanalyzer-transitivity
endif
endif

STRIP_KERNEL ?= true
STRIP_KERNEL_FLAGS ?= --strip-debug --discard-locals

ifeq ($(STRIP_KERNEL), true)
define kernel_strip
	@$(ST) $(STRIP_KERNEL_FLAGS) $(strip $(1))
endef
else
define kernel_strip
endef
endif

CC_BASE_TRACE :=
ifeq ($(TRACEABLE_KERNEL), true)
CC_BASE_TRACE := -g -fno-omit-frame-pointer
endif

CC_BASE := \
	$(CC_BASE) \
	-Wno-unused-parameter \
	-Wno-missing-braces \
	-DVERSION=\"$(VERSION)\" \
	-DBUILD_DATE=\"$(BUILD_DATE)\" \
	-DGIT_COMMIT=\"$(GIT_COMMIT_SHORT)\" \
	$(CC_BASE_ANALYZER) \
	$(CC_BASE_TRACE) \
	$(CC_BASE_PROFILE)

# Returns the runtime helper archive for the given CFLAGS, or empty if missing.
# Some Clang packages print a compiler-rt path they do not ship, so RISC-V
# builds also try the bare-metal GCC toolchain when it is installed.
LIBGCC = $(shell \
	lib=$$($(CC) $(CC_BASE) $(1) -print-libgcc-file-name 2>/dev/null); \
	if [ -f "$$lib" ]; then echo "$$lib"; exit 0; fi; \
	for cc in $(LIBGCC_FALLBACK_CC); do \
		if ! command -v "$$cc" >/dev/null 2>&1; then continue; fi; \
		lib=$$($$cc $(CC_BASE) $(1) -print-libgcc-file-name 2>/dev/null); \
		if [ -f "$$lib" ]; then echo "$$lib"; exit 0; fi; \
	done)

ifneq ($(ARCH),)
include kernel/arch/$(ARCH_TREE)/build/build.mk
endif
