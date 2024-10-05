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
	-std=gnu23 \
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

ASM_BASE := \
	-Wall \
	-w-reloc-abs

LD_BASE := \
	-nostdlib \
	-z max-page-size=0x1000 \
	-z noexecstack


include build/toolchain.mk
include build/$(TARGET).mk

include boot/build.mk
include kernel/build.mk

include utils/docker.mk
include utils/qemu.mk
include utils/font.mk


.DEFAULT_GOAL := all
.PHONY: all
all: $(TARGET)
	$(info Build completed successfully!)

.PHONY: clean
clean:
	rm -rf bin
