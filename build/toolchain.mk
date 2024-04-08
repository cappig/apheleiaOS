AS := nasm
OC := objcopy
ST := strip

ifeq ($(TOOLCHAIN), gnu)
	CC := gcc
	LD := ld
else ifeq ($(TOOLCHAIN), llvm)
	CC := clang
	LD := ld.lld
endif

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

ifeq ($(PROFILE), debug)
	CC_BASE += -Og -g
else ifeq ($(PROFILE), small)
	CC_BASE += -Os
else ifeq ($(PROFILE), normal)
	CC_BASE += -O2
else ifeq ($(PROFILE), fast)
	CC_BASE += -O3
endif
