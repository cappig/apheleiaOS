
ARCH_DIR := kernel/arch/x86

include kernel/arch/x86/boot/build.mk
include kernel/build.mk


bin/$(IMG_NAME): bin/boot/bios.bin bin/boot/mbr.bin bin/image/kernel.elf
	@mkdir -p $(@D)
	@cp -r root/* bin/image
	@kernel/arch/x86/image.sh $@ $< bin/image
	@kernel/arch/x86/mbr.sh bin/boot/mbr.bin $@
