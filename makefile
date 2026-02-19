NAME    := apheleia
VERSION := alpha-0.1

BUILD_DATE       ?= $(shell date -u +%Y-%m-%d)
GIT_COMMIT_SHORT ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

ARCH             := x86_64
TOOLCHAIN        ?= gnu
PROFILE          ?= fast
IMAGE_FORMAT     ?= img
TRACEABLE_KERNEL ?= true

BUILD_NAME := $(NAME)_$(VERSION)
IMAGE_NAME := $(BUILD_NAME)_$(ARCH)

ifeq ($(filter $(IMAGE_FORMAT),img iso),)
$(error Unsupported IMAGE_FORMAT '$(IMAGE_FORMAT)'; expected 'img' or 'iso')
endif

CC_BASE := \
	-MD \
	-Wall \
	-Wextra \
	-Wshadow \
	-std=gnu2x \
	-Ilibs \
	-Ilibs/libc \
	-Ikernel \
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
	-z noexecstack

include utils/toolchain.mk
include utils/qemu.mk
include user/build.mk

.DEFAULT_GOAL := all
.PHONY: all clean

all: bin/$(IMAGE_NAME).$(IMAGE_FORMAT)
	@echo "Build completed successfully!"

clean:
	@rm -rf bin
	@echo "Build directories cleaned"
