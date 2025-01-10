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

CC_BASE += \
	-Wno-unused-parameter \
	-Wno-missing-braces \
	-DVERSION=\"$(VERSION)\"

CC_DEBUG := \
	-DDISK_DEBUG \
	-DINPUT_DEBUG \
	-DMMU_DEBUG

CC_DEBUG_EXTRA := \
	-DINT_DEBUG \
	-DKMALLOC_DEBUG \
	-DSYSCALL_DEBUG \
	-DSCHED_DEBUG

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

ifeq ($(TRACEABLE_KERNEL), true)
	CC_BASE += -g -fno-omit-frame-pointer
endif

ifeq ($(PROFILE), debug)
	CC_BASE += -Og $(CC_DEBUG)
else ifeq ($(PROFILE), debug_extra)
	CC_BASE += -Og $(CC_DEBUG) $(CC_DEBUG_EXTRA)
else ifeq ($(PROFILE), small)
	CC_BASE += -Os
else ifeq ($(PROFILE), normal)
	CC_BASE += -O2
else ifeq ($(PROFILE), fast)
	CC_BASE += -O3
endif
