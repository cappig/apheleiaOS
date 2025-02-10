INITRD_SRC := bin/initrd

$(INITRD_SRC)/boot/font.psf:
	@mkdir -p $(@D)
	cp utils/$(DEFAULT_FONT) $@

$(INITRD_SRC)/boot/sym.map: bin/image/kernel.elf
ifeq ($(TRACEABLE_KERNEL), true)
	@mkdir -p $(@D)
	cp bin/kernel/sym.map $@
endif


bin/image/initrd.tar: bin/initrd/boot/sym.map bin/initrd/boot/font.psf
	tar -cf $@ -C $(INITRD_SRC) \
	$(shell ls -A $(INITRD_SRC))
