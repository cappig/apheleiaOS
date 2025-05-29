AS := nasm
OC := objcopy
ST := strip
NM := nm

ifeq ($(TOOLCHAIN), gnu)
	CC := gcc
	LD := ld
else ifeq ($(TOOLCHAIN), llvm)
	CC := clang
	LD := ld.lld
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

CC_BASE += \
	-Wno-unused-parameter \
	-Wno-missing-braces \
	-DVERSION=\"$(VERSION)\"

CC_DEBUG := \
	-DDISK_DEBUG \
	-DINPUT_DEBUG \
	-DMMU_DEBUG

CC_DEBUG_EXTRA := \
	-DKMALLOC_DEBUG \
	-DSCHED_DEBUG \
	-DINT_DEBUG \
	-DSYSCALL_DEBUG

# scan-build is a nice clang alternative
GCC_ANALYZER ?= true

ifeq ($(TOOLCHAIN), gnu)
ifeq ($(GCC_ANALYZER), true)
	CC_BASE += \
		-fanalyzer \
		-fanalyzer-transitivity
endif
endif

# If we want to be able to perform reliable stack tracing in the kernel we have
# to load a symbol table and compile without omitting frame pointers
TRACEABLE_KERNEL ?= true

STRIP_KERNEL ?= false

ifeq ($(TRACEABLE_KERNEL), true)
	CC_BASE += -g -fno-omit-frame-pointer
	STRIP_KERNEL = false
endif

ifeq ($(PROFILE), debug)
	CC_BASE += -Og $(CC_DEBUG)
	TRACEABLE_KERNEL = true
else ifeq ($(PROFILE), debug_extra)
	CC_BASE += -Og $(CC_DEBUG) $(CC_DEBUG_EXTRA)
else ifeq ($(PROFILE), small)
	CC_BASE += -Os
else ifeq ($(PROFILE), normal)
	CC_BASE += -O2
else ifeq ($(PROFILE), fast)
	CC_BASE += -O3
endif
