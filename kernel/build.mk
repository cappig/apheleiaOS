KERNEL_SRC := \
	$(shell find kernel/ -type f -name '*.asm') \
	$(shell find kernel/ -type f -name '*.c') \
	$(addprefix kernel/, $(wildcard libs/**/*.c))

KERNEL_OBJ := $(patsubst %, bin/%.o, $(KERNEL_SRC))

CC_KERNEL := \
	-Ikernel \
	-fdata-sections \
	-ffunction-sections \
	-DHAS_GMALLOC \
	-march=x86-64 \
	-mcmodel=kernel \
	-m64

LD_KERNEL := \
	-Tkernel/linker.ld \
	-melf_x86_64 \
	--gc-sections

AS_KERNEL := \
	-f elf64

bin/kernel/%.asm.o: kernel/%.asm
	@mkdir -p $(@D)
	$(call as, $(AS_KERNEL), $@, $<)

bin/kernel/%.c.o: kernel/%.c
	@mkdir -p $(@D)
	$(call cc, $(CC_KERNEL), $@, $<)

bin/kernel/libs/%.c.o: libs/%.c
	@mkdir -p $(@D)
	$(call cc, $(CC_KERNEL), $@, $<)


bin/image/kernel.elf: $(KERNEL_OBJ)
	$(call ld, $(LD_KERNEL), $@, $^)
ifeq ($(TRACEABLE_KERNEL), true)
	$(call nm, $@, bin/kernel/sym.map)
endif
	$(call st, $@)


.PHONY: clean_kernel
clean_kernel:
	rm -rf bin/kernel
