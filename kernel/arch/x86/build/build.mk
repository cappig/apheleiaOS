ARCH_DIR := kernel/arch/x86

KERNEL_SRC_DIRS := \
	kernel \
	$(filter-out kernel/arch, $(wildcard kernel/*)) \
	$(ARCH_DIR) \
	$(filter-out $(ARCH_DIR)/boot, $(wildcard $(ARCH_DIR)/*)) \
	$(LIBC_DIRS) \
	libs/log \
	libs/alloc \
	libs/data \
	libs/parse

KERNEL_ALL_SRC := $(foreach dir, $(KERNEL_SRC_DIRS), $(wildcard $(dir)/*.c) $(wildcard $(dir)/*.asm))

KERNEL_COMMON_SRC := $(filter-out %32.c %64.c %32.asm %64.asm, $(KERNEL_ALL_SRC))
KERNEL_SRC_64 := $(filter %64.c %64.asm, $(KERNEL_ALL_SRC)) $(KERNEL_COMMON_SRC)
KERNEL_SRC_32 := $(filter %32.c %32.asm, $(KERNEL_ALL_SRC)) $(KERNEL_COMMON_SRC)

include kernel/arch/x86/boot/build.mk

ifeq ($(ARCH_VARIANT), 64)
KERNEL_SRC := $(KERNEL_SRC_64)
KERNEL_ELF := bin/image/boot/kernel64.elf
include kernel/arch/x86/build/build64.mk
else ifeq ($(ARCH_VARIANT), 32)
KERNEL_SRC := $(KERNEL_SRC_32)
KERNEL_ELF := bin/image/boot/kernel32.elf
include kernel/arch/x86/build/build32.mk
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)')
endif


bin/$(IMG_NAME): bin/boot/bios.bin bin/boot/mbr.bin $(KERNEL_ELF)
	@mkdir -p $(@D)
	@cp $(KERNEL_ELF) bin/image/boot/kernel.elf
	@cp -r root/* bin/image
	@kernel/image.sh $@ $< bin/image
	@kernel/arch/x86/build/mbr.sh bin/boot/mbr.bin $@
