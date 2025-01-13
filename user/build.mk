USER_BIN := bin/initrd/usr


CC_USER := \
	$(CC_BASE) \
	-static

LD_USER := \
	$(LD_BASE) \
	-melf_x86_64 \
	--gc-sections

ULIB_SRC := \
	$(wildcard libs/libc/*.c) \
	$(wildcard libs/libc_ext/*.c) \
	$(wildcard libs/libc_usr/*.c) \
	$(wildcard libs/libc_usr/*.asm)


bin/user/%.c.o: user/%.c
	@mkdir -p $(@D)
	$(CC) $(CC_USER) -c -o $@ $<

bin/user/%.asm.o: user/%.asm
	@mkdir -p $(@D)
	$(AS) $(ASM_BASE) -f elf64 -o $@ $<

bin/user/libs/%.c.o: libs/%.c
	@mkdir -p $(@D)
	$(CC) $(CC_USER) -c -o $@ $<

bin/user/libs/%.asm.o: libs/%.asm
	@mkdir -p $(@D)
	$(AS) $(ASM_BASE) -f elf64 -o $@ $<


include $(wildcard user/**/build.mk)

.PHONY: user
user: $(USER_OBJS)

.PHONY: clean_user
clean_user:
	rm -rf $(USER_BIN)
