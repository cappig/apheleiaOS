AS := nasm
AR := ar
OC := objcopy
ST := strip

pick_tool = $(shell utils/pick_tool.sh $(1))
gcc_tool  = $(shell utils/gcc_tool.sh "$(strip $(1))" $(2) $(3))

# Split ARCH into a family and word size, then load target-owned settings.
ARCH_TREE    := $(word 1, $(subst _, ,$(ARCH)))
ARCH_VARIANT := $(word 2, $(subst _, ,$(ARCH)))

LIBGCC_FALLBACK_CC ?=

include utils/arch/$(ARCH_TREE)/toolchain.mk

TOOLCHAIN_FREE_GOALS := clean docker_image docker_build
TARGET_TOOL_GOALS := $(filter-out $(TOOLCHAIN_FREE_GOALS),$(MAKECMDGOALS))
VALIDATE_TARGET_TOOLS := $(if $(MAKECMDGOALS),$(if $(TARGET_TOOL_GOALS),true,false),true)

ifneq ($(ARCH),)

ifeq ($(TOOLCHAIN), gnu)
AR := $(GNU_AR_$(ARCH))
CC := $(GNU_CC_$(ARCH))
LD := $(GNU_LD_$(ARCH))
OC := $(GNU_OC_$(ARCH))
ST := $(GNU_ST_$(ARCH))
else ifeq ($(TOOLCHAIN), llvm)
AR := $(LLVM_AR_$(ARCH))
CC := $(LLVM_CC_$(ARCH))
LD := $(LLVM_LD_$(ARCH))
OC := $(LLVM_OC_$(ARCH))
ST := $(LLVM_ST_$(ARCH))
else
$(error Unsupported TOOLCHAIN '$(TOOLCHAIN)')
endif

ifeq ($(VALIDATE_TARGET_TOOLS),true)
ifeq ($(strip $(CC)),)
$(error Missing compiler for ARCH '$(ARCH)' and TOOLCHAIN '$(TOOLCHAIN)')
endif
ifeq ($(strip $(LD)),)
$(error Missing linker for ARCH '$(ARCH)' and TOOLCHAIN '$(TOOLCHAIN)')
endif
ifeq ($(strip $(AR)),)
$(error Missing archiver for ARCH '$(ARCH)' and TOOLCHAIN '$(TOOLCHAIN)')
endif
ifeq ($(strip $(OC)),)
$(error Missing objcopy for ARCH '$(ARCH)' and TOOLCHAIN '$(TOOLCHAIN)')
endif
ifeq ($(strip $(ST)),)
$(error Missing strip for ARCH '$(ARCH)' and TOOLCHAIN '$(TOOLCHAIN)')
endif
endif

endif

tool_version = $(if $(strip $(1)),$(shell { $(1) --version 2>/dev/null || $(1) -v 2>/dev/null || true; } | sed -n '1,3p' | tr '\n' ' '))

TOOLCHAIN_CONFIG := \
	CC_VERSION="$(call tool_version,$(CC))" \
	AS_VERSION="$(call tool_version,$(AS))" \
	AR_VERSION="$(call tool_version,$(AR))" \
	LD_VERSION="$(call tool_version,$(LD))" \
	OC_VERSION="$(call tool_version,$(OC))" \
	ST_VERSION="$(call tool_version,$(ST))"

define log
	@printf "%-3s  %s\n" "$(strip $(1))" "$(strip $(2))"
endef

# rebuild objects when command-line flags change. Make tracks source
# dependencies, but it does not know when the recipe text has changed
.PHONY: FORCE
FORCE:

define flag_stamp
$(1): FORCE
	@utils/write_flag_stamp.sh "$$@" '$(strip $($(2)))'
endef

# compiler / assembler / linker wrappers used by arch build.mk files
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

LIBC_DIRS := libs/libc libs/libc_ext libs/arch/$(ARCH_TREE)

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

BUILD_DATE       ?= $(shell date -u $(if $(SOURCE_DATE_EPOCH),--date=@$(SOURCE_DATE_EPOCH)) +%Y-%m-%d)
GIT_COMMIT_SHORT ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

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

# returns the runtime helper archive for the given CFLAGS, or empty if missing
# Some Clang packages report a compiler-rt path they do not ship. Target
# configuration may provide fallback compilers for the runtime archive.
LIBGCC = $(if $(strip $(CC)),$(shell \
	lib=$$($(CC) $(CC_BASE) $(1) -print-libgcc-file-name 2>/dev/null); \
	if [ -f "$$lib" ]; then echo "$$lib"; exit 0; fi; \
	for cc in $(LIBGCC_FALLBACK_CC); do \
		if ! command -v "$$cc" >/dev/null 2>&1; then continue; fi; \
		lib=$$($$cc $(CC_BASE) $(1) -print-libgcc-file-name 2>/dev/null); \
		if [ -f "$$lib" ]; then echo "$$lib"; exit 0; fi; \
	done))

ifneq ($(ARCH),)
include kernel/arch/$(ARCH_TREE)/build/build.mk
endif
