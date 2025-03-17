NAME := apheleia
VERSION := pre-alpha

TARGET := x86_iso

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
	-ffreestanding \
	-fno-stack-protector \
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
	-z max-page-size=0x1000 \
	-z noexecstack


include build/toolchain.mk
include build/$(TARGET).mk

include boot/build.mk
include kernel/build.mk
include user/build.mk

include utils/docker.mk
include utils/qemu.mk
include utils/font.mk
include utils/check.mk


.DEFAULT_GOAL := all
.PHONY: all
all: user $(TARGET)
	@echo "Build completed successfully!"

.PHONY: clean
clean:
	@rm -rf bin
	@echo "Build directories cleaned"
