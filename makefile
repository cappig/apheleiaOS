NAME := apheleia
VERSION := pre-alpha

ARCH := x86_64

BUILD_NAME := $(NAME)_$(VERSION)
IMG_NAME := $(BUILD_NAME)_$(ARCH).img

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


.DEFAULT_GOAL := all
.PHONY: all
all: bin/$(IMG_NAME)
	@echo "Build completed successfully!"

.PHONY: clean
clean:
	@python -c "import shutil; shutil.rmtree('bin', ignore_errors=True)"
	@echo "Build directories cleaned"
