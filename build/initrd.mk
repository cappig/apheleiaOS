bin/initrd/font.psf:
	@mkdir -p $(@D)
	cp utils/$(DEFAULT_FONT) $@

bin/initrd/sym.map: bin/image/kernel.elf
ifeq ($(TRACEABLE_KERNEL), true)
	@mkdir -p $(@D)
	cp bin/kernel/sym.map $@
endif


bin/image/initrd.tar: bin/initrd/sym.map bin/initrd/font.psf
	tar -cf $@ -C $(<D) \
	$(shell ls -A $(<D))
