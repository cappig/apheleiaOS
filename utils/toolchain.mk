AS := nasm
OC := objcopy
ST := strip
NM := nm

# This evaluates to the directory of the calling makefile
MAKE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

GNU_CC ?= gcc
GNU_CC_x86_64 ?= x86_64-linux-gnu-gcc
GNU_CC_x86_32 ?= $(GNU_CC)

LLVM_CC ?= clang
LLVM_CC_x86_64 ?= $(LLVM_CC)
LLVM_CC_x86_32 ?= $(LLVM_CC)

ifeq ($(TOOLCHAIN), gnu)
	CC := $(GNU_CC)
ifeq ($(ARCH), x86_64)
	CC := $(GNU_CC_x86_64)
else ifeq ($(ARCH), x86_32)
	CC := $(GNU_CC_x86_32)
endif
	LD := ld
else ifeq ($(TOOLCHAIN), llvm)
	CC := $(LLVM_CC)
ifeq ($(ARCH), x86_64)
	CC := $(LLVM_CC_x86_64)
else ifeq ($(ARCH), x86_32)
	CC := $(LLVM_CC_x86_32)
endif
	LD := ld.lld
else
$(error Unsupported TOOLCHAIN '$(TOOLCHAIN)')
endif

define cc
	@$(CC) $(CC_BASE) $(1) -c -o $(2) $(3)
	@echo "CC $(3)"
endef

define as
	@$(AS) $(AS_BASE) $(1) -o $(2) $(3)
	@echo "AS $(3)"
endef

define ld
	@$(LD) $(LD_BASE) $(1) -o $(2) $(3)
	@echo "LD $(2)"
endef

define oc
	@$(OC) $(1) $(2) $(3)
	@echo "OC $(2)"
endef

define st
	@$(ST) $(1)
	@echo "ST $(1)"
endef

define nm
	@$(NM) $(1) > $(2)
	@echo "NM $(2)"
endef

# Link against libgcc for common builtins
LIBGCC = $(shell $(CC) $(CC_BASE) $(1) -print-libgcc-file-name)

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
	-DMMU_DEBUG \
	-g

CC_DEBUG_EXTRA := \
	-DKMALLOC_DEBUG \
	-DSCHED_DEBUG \
	-DINT_DEBUG \
	-DSYSCALL_DEBUG

# In the arch string everything after the first '_' is treated as a variant of the base tree.
# Example: x86_64 maps to the x86 tree with the 64-bit variant.
ARCH_TREE := $(word 1, $(subst _, ,$(ARCH)))
ARCH_VARIANT := $(word 2, $(subst _, ,$(ARCH)))

include kernel/arch/$(ARCH_TREE)/build/build.mk

# GCC static analyzer
GCC_ANALYZER ?= false

CC_BASE_ANALYZER :=
ifeq ($(TOOLCHAIN), gnu)
ifeq ($(GCC_ANALYZER), true)
CC_BASE_ANALYZER := \
	-fanalyzer \
	-fanalyzer-transitivity
endif
endif

# If we want reliable stack tracing we need symbols and frame pointers.
TRACEABLE_KERNEL ?= true

STRIP_KERNEL ?= false

CC_BASE_TRACE :=
ifeq ($(TRACEABLE_KERNEL), true)
CC_BASE_TRACE := \
	-g \
	-fno-omit-frame-pointer
	STRIP_KERNEL = false
endif

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

CC_BASE := \
	$(CC_BASE) \
	$(CC_BASE_COMMON) \
	$(CC_BASE_ANALYZER) \
	$(CC_BASE_TRACE) \
	$(CC_BASE_PROFILE)
