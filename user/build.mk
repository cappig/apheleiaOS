USER_BIN := bin/initrd/sbin


CC_USER := \
	-static \
	-Ilibs/libc

LD_USER := \
	-melf_x86_64 \
	--gc-sections

AS_USER := \
	-f elf64

ULIB_SRC := \
	$(wildcard libs/libc/*.c) \
	$(wildcard libs/libc_ext/*.c) \
	$(wildcard libs/libc_usr/*.c) \
	$(wildcard libs/libc_usr/*.asm)


bin/user/%.c.o: user/%.c
	@mkdir -p $(@D)
	$(call cc, $(CC_USER), $@, $<)

bin/user/%.asm.o: user/%.asm
	@mkdir -p $(@D)
	$(call as, $(AS_USER), $@, $<)

bin/user/libs/%.c.o: libs/%.c
	@mkdir -p $(@D)
	$(call cc, $(CC_USER), $@, $<)

bin/user/libs/%.asm.o: libs/%.asm
	@mkdir -p $(@D)
	$(call as, $(AS_USER), $@, $<)


include $(wildcard user/**/build.mk)

.PHONY: user
user: $(USER_OBJS)

.PHONY: clean_user
clean_user:
	rm -rf $(USER_BIN)
