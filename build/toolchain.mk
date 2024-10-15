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

ifeq ($(TOOLCHAIN), gnu)
	CC_BASE += \
		-fanalyzer
else ifeq ($(TOOLCHAIN), llvm)
	CC_BASE += \
		-Wno-language-extension-token \
		-Wno-fixed-enum-extension \
		-Wno-gnu-binary-literal \
		-Wno-gnu-case-range \
		-Wno-gnu-union-cast \
		-Wno-gnu-statement-expression
endif

# If we want to be able to perform stack tracing in the kernel we have
# to load a symbol table and compile witouth ommiting frame pointers
TRACEABLE_KERNEL ?= true

ifeq ($(TRACEABLE_KERNEL), true)
	CC_BASE += -g -fno-omit-frame-pointer
endif

ifeq ($(PROFILE), debug)
	CC_BASE += -Og -DDISK_DEBUG -DKMALLOC_DEBUG -DPS2_DEBUG
else ifeq ($(PROFILE), small)
	CC_BASE += -Os
else ifeq ($(PROFILE), normal)
	CC_BASE += -O2
else ifeq ($(PROFILE), fast)
	CC_BASE += -O3
endif
