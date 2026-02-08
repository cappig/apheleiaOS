KERNEL32_OBJ := $(patsubst %, bin/kernel32/%.o, $(KERNEL_SRC))

AS_KERNEL32 := -felf32
CC_KERNEL32 := \
	-fdata-sections \
	-DEXTERNAL_ALLOC \
	-ffunction-sections \
	-m32
LD_KERNEL32 := \
	--gc-sections \
	-T$(ARCH_DIR)/build/linker32.ld

bin/kernel32/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(AS_KERNEL32), $@, $<)

bin/kernel32/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(CC_KERNEL32), $@, $<)


bin/image/boot/kernel32.elf: $(KERNEL32_OBJ) $(call LIBGCC, $(CC_KERNEL))
	@mkdir -p $(@D)
	$(call ld, $(LD_KERNEL32), $@, $^)
