BOOT_SRC := \
	$(wildcard boot/bios/*.asm) \
	$(wildcard boot/bios/*.c) \
	$(addprefix boot/, $(wildcard libs/base/*.c)) \
	$(addprefix boot/, $(wildcard libs/libc/*.c)) \
	$(addprefix boot/, $(wildcard libs/libc_ext/*.c)) \
	$(addprefix boot/, $(wildcard libs/parse/*.c)) \
	$(addprefix boot/, $(wildcard libs/x86/*.c))

BOOT_OBJ := $(patsubst %, bin/%.o, $(BOOT_SRC))

MBR_SRC := $(wildcard boot/mbr/*.asm)
MBR_OBJ := $(patsubst %, bin/%.o, $(MBR_SRC))

CC_BOOT := \
	$(CC_BASE) \
	-fdata-sections \
	-ffunction-sections \
	-m32

LD_BOOT := \
	$(LD_BASE) \
	--gc-sections

bin/boot/%.asm.o: boot/%.asm
	@mkdir -p $(@D)
	$(AS) $(ASM_BASE) -f elf32 -o $@ $<

bin/boot/%.c.o: boot/%.c
	@mkdir -p $(@D)
	$(CC) $(CC_BOOT) -c -o $@ $<

bin/boot/libs/%.c.o: libs/%.c
	@mkdir -p $(@D)
	$(CC) $(CC_BOOT) -c -o $@ $<

bin/image/root/mbr.bin: $(MBR_OBJ)
	@mkdir -p $(@D)
	$(LD) $(LD_BASE) --oformat=binary -Tboot/mbr/linker.ld -o $@ $^

bin/image/root/boot.bin: $(BOOT_OBJ)
	@mkdir -p $(@D)
	$(LD) $(LD_BOOT) -Tboot/bios/linker.ld -o bin/boot/boot.elf $^
	$(OC) -O binary bin/boot/boot.elf $@

