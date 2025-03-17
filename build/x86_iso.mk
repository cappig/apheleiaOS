IMG_NAME := $(NAME).iso


bin/image/args.cfg:
	@cp build/args.cfg $@

# xorriso prints the version info to stderr, and there is apparently _no way_ to disable this
# complete and utter fucking bullshit...
bin/$(IMG_NAME): bin/image/mbr.bin
	@xorriso -as mkisofs -quiet -J -r -no-pad \
		-V $(NAME) -c boot.cat -b boot.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--grub2-mbr $< \
		-o $@ bin/image


include build/initrd.mk

.PHONY: x86_iso
x86_iso: \
	bin/image/boot.bin \
	bin/image/initrd.tar \
	bin/image/args.cfg \
	bin/$(IMG_NAME)
