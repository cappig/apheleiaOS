IMG_NAME := $(NAME).iso

bin/image/root/initrd.tar:
	tar -cf $@ -C build/initrd \
	$(shell ls -A build/initrd)

bin/image/root/args.cfg:
	cp build/args.cfg $@

bin/$(IMG_NAME): bin/image/root/mbr.bin
	xorriso -as mkisofs -quiet -J -r -no-pad \
		-V $(NAME) -c boot.cat -b boot.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--grub2-mbr $< -chrp-boot \
		-o $@ bin/image/root


.PHONY: x86_iso
x86_iso: \
	bin/image/root/boot.bin \
	bin/image/root/kernel.elf \
	bin/image/root/initrd.tar \
	bin/image/root/args.cfg \
	bin/$(IMG_NAME)
