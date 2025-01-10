IMG_NAME := $(NAME).iso


bin/image/args.cfg:
	cp build/args.cfg $@

bin/$(IMG_NAME): bin/image/mbr.bin
	xorriso -as mkisofs -quiet -J -r -no-pad \
		-V $(NAME) -c boot.cat -b boot.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--grub2-mbr $< -chrp-boot \
		-o $@ bin/image


include build/initrd.mk

.PHONY: x86_iso
x86_iso: \
	bin/image/boot.bin \
	bin/image/initrd.tar \
	bin/image/args.cfg \
	bin/$(IMG_NAME)
