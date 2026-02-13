NAME := apheleia
VERSION := pre-alpha
BUILD_DATE ?= $(shell date -u +%Y-%m-%d)
GIT_COMMIT_SHORT ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

ARCH := x86_64

BUILD_NAME := $(NAME)_$(VERSION)
IMG_NAME := $(BUILD_NAME)_$(ARCH).img
IMAGE_FORMAT ?= img

ifeq ($(IMAGE_FORMAT), img)
IMAGE_NAME := $(IMG_NAME)
else ifeq ($(IMAGE_FORMAT), iso)
IMAGE_NAME := $(BUILD_NAME)_$(ARCH).iso
else
$(error Unsupported IMAGE_FORMAT '$(IMAGE_FORMAT)'; expected 'img' or 'iso')
endif

TOOLCHAIN ?= gnu
PROFILE ?= fast
TRACEABLE_KERNEL ?= true

# Shared C compiler flags
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

# Shared assembler flags
AS_BASE := \
	-Wall \
	-w-reloc-abs \
	-w-reloc-rel-dword \
	-w-label-orphan

# Shared linker flags
LD_BASE := \
	-z noexecstack

include utils/toolchain.mk
include utils/qemu.mk
include user/build.mk

.DEFAULT_GOAL := all
.PHONY: all
all: bin/$(IMAGE_NAME) $(SYMBOL_MAP)
	@echo "Build completed successfully!"

.PHONY: clean
clean:
	@rm -rf bin
	@echo "Build directories cleaned"
