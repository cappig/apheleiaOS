IMG_NAME := $(NAME).img

ARCH_DIR := kernel/arch/x86

include kernel/arch/x86/boot/build.mk

# xorriso prints the version info to stderr, and there is apparently _no way_ to disable this
# complete and utter fucking bullshit...
# bin/$(IMG_NAME): bin/image/mbr.bin
# 	@xorriso -as mkisofs -quiet -J -r -no-pad \
# 		-V $(NAME) -c boot.cat -b boot.bin \
# 		-no-emul-boot -boot-load-size 4 -boot-info-table \
# 		--grub2-mbr $< \
# 		-o $@ bin/image


# include build/initrd.mk



bin/$(IMG_NAME): bin/boot/mbr.bin bin/image/boot/bios.bin
	@mkdir -p $(@D)
	@utils/mbr.sh $@ $< bin/image

.PHONY: x86
x86: \
	bin/boot/mbr.bin \
	bin/$(IMG_NAME)
