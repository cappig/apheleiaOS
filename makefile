NAME := ApheleiaOS
VERSION := pre-alpha

TARGET := x86_hdd

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

ASM_BASE := \
	-Wall

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


.DEFAULT_GOAL := all
.PHONY: all
all: $(TARGET)

.PHONY: clean
clean:
	rm -rf bin
