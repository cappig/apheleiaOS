KERNEL64_OBJ := $(patsubst %, bin/kernel64/%.o, $(KERNEL_SRC))

AS_KERNEL64 := -felf64
CC_KERNEL64 := \
	-fdata-sections \
	-D_KERNEL \
	-DEXTERNAL_ALLOC \
	-ffunction-sections \
	-march=x86-64 \
	-mcmodel=kernel \
	-m64
LD_KERNEL64 := \
	--gc-sections \
	-T$(ARCH_DIR)/build/linker64.ld

bin/kernel64/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(AS_KERNEL64), $@, $<)

bin/kernel64/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(CC_KERNEL64), $@, $<)


$(KERNEL_ELF): $(KERNEL64_OBJ) $(call LIBGCC, $(CC_KERNEL64))
	@mkdir -p $(@D)
	$(call ld, $(LD_KERNEL64), $@, $^)
