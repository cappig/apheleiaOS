ARCH_DIR := kernel/arch/x86

KERNEL_SRC_DIRS := \
	kernel \
	$(filter-out arch, $(wildcard kernel/*)) \
	$(filter-out boot, $(wildcard $(ARCH_DIR)/*)) \
	$(LIBC_DIRS) \
	libs/alloc \
	libs/data \
	libs/parse

KERNEL_ALL_SRC := $(foreach dir, $(KERNEL_SRC_DIRS), $(wildcard $(dir)/*.c) $(wildcard $(dir)/*.asm))

# Only build with files for the slected variant -- 32 or 64 bit
# files that do not end in either 32 or 64 will be included in all builds
KERNEL_SRC := $(filter %$(ARCH_VARIANT).c %$(ARCH_VARIANT).asm, $(KERNEL_ALL_SRC))
KERNEL_SRC += $(filter-out %32.c %64.c %32.asm %64.asm, $(KERNEL_ALL_SRC))

include kernel/arch/x86/boot/build.mk

include kernel/arch/x86/build64.mk
# include kernel/arch/x86/build32.mk


bin/$(BUILD_NAME)_x86_64.img: bin/boot/bios.bin bin/boot/mbr.bin bin/image/boot/kernel64.elf
	@mkdir -p $(@D)
	@mv bin/image/boot/kernel$(ARCH_VARIANT).elf bin/image/boot/kernel.elf
	@cp -r root/* bin/image
	@kernel/image.sh $@ $< bin/image
	@kernel/arch/x86/mbr.sh bin/boot/mbr.bin $@
