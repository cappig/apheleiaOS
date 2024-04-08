KERNEL_SRC := \
	$(shell find kernel/ -type f -name '*.asm') \
	$(shell find kernel/ -type f -name '*.c')

KERNEL_OBJ := $(patsubst %, bin/%.o, $(KERNEL_SRC))

CC_KERNEL := \
	$(CC_BASE) \
	-fdata-sections \
	-ffunction-sections \
	-march=x86-64 \
	-mcmodel=kernel \
	-m64

LD_KERNEL := \
	$(LD_BASE) \
	-melf_x86_64 \
	--gc-sections


bin/kernel/%.asm.o: kernel/%.asm
	@mkdir -p $(@D)
	$(AS) $(ASM_BASE) -f elf64 -o $@ $<

bin/kernel/%.c.o: kernel/%.c
	@mkdir -p $(@D)
	$(CC) $(CC_KERNEL) -c -o $@ $<

bin/kernel/libs/%.c.o: libs/%.c
	@mkdir -p $(@D)
	$(CC) $(CC_KERNEL) -c -o $@ $<


bin/image/root/kernel.elf: $(KERNEL_OBJ)
	$(LD) $(LD_KERNEL) -Tkernel/linker.ld -o $@ $^
