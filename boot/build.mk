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
	-fdata-sections \
	-ffunction-sections \
	-m32

LD_BOOT := \
	--gc-sections

AS_BOOT := \
	-f elf32

bin/boot/%.asm.o: boot/%.asm
	@mkdir -p $(@D)
	$(call as, $(AS_BOOT), $@, $<)

bin/boot/%.c.o: boot/%.c
	@mkdir -p $(@D)
	$(call cc, $(CC_BOOT), $@, $<)

bin/boot/libs/%.c.o: libs/%.c
	@mkdir -p $(@D)
	$(call cc, $(CC_BOOT), $@, $<)

bin/image/mbr.bin: $(MBR_OBJ)
	@mkdir -p $(@D)
	$(call ld, --oformat=binary -Tboot/mbr/linker.ld, $@, $^)

bin/image/boot.bin: $(BOOT_OBJ)
	@mkdir -p $(@D)
	$(call ld, $(LD_BOOT) -Tboot/bios/linker.ld, bin/boot/boot.elf, $^)
	$(call oc, -O binary, bin/boot/boot.elf, $@)


.PHONY: clean_bootloader
clean_bootloader:
	rm -rf bin/boot
