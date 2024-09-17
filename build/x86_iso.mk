IMG_NAME := $(NAME).iso

.PHONY: x86_iso
x86_iso: bin/$(IMG_NAME)

bin/$(IMG_NAME): bin/image/root/mbr.bin bin/image/root/boot.bin bin/image/root/kernel.elf
	xorriso -as mkisofs -quiet -J -r -no-pad \
		-V $(NAME) -c boot.cat -b boot.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--grub2-mbr $< -chrp-boot \
		-o $@ bin/image/root
