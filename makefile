NAME := apheleia
VERSION := pre-alpha

ARCH := x86

TOOLCHAIN ?= gnu
PROFILE ?= fast

CC_BASE := \
	-MD \
	-Wall \
	-Wextra \
	-Wshadow \
	-std=gnu2x \
	-Ilibs \
	-Ilibs/libc \
	-Ikernel/arch \
	-ffreestanding \
	-fno-stack-protector \
	-DEXTEND_LIBC \
	-nostdinc \
	-mno-sse \
	-mno-sse2 \
	-mno-mmx \
	-mno-red-zone \
	-fno-pic \
	-fno-pie

AS_BASE := \
	-Wall \
	-w-reloc-abs \
	-w-reloc-rel-dword \
	-w-label-orphan

LD_BASE := \
	-nostdlib \
	-z noexecstack


include build/toolchain.mk
include build/$(ARCH).mk

# include boot/build.mk
# include kernel/build.mk
# include user/build.mk
#
# include utils/docker.mk
include utils/qemu.mk
# include utils/font.mk
# include utils/check.mk


.DEFAULT_GOAL := all
.PHONY: all
all: $(ARCH)
	@echo "Build completed successfully!"

.PHONY: clean
clean:
	@rm -rf bin
	@echo "Build directories cleaned"
