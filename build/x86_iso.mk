IMG_NAME := $(NAME).iso

.PHONY: x86_iso
x86_iso: bin/$(IMG_NAME)

#dd status=none conv=notrunc if=$< of=$@

bin/$(IMG_NAME): bin/image/root/mbr.bin bin/image/root/boot.bin
	xorriso -as mkisofs -quiet -J -r -no-pad \
		-V $(NAME) -c boot.cat \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--grub2-mbr $< \
		-b boot.bin -o $@ bin/image/root
