MBR_DIR  := $(MAKE_DIR)/mbr
BIOS_DIR := $(MAKE_DIR)/bios
LIB_DIRS  := $(ARCH_DIR) kernel/lib

SRC_DIRS := \
	$(BIOS_DIR) \
	$(LIB_DIRS) \
	$(LIBC_DIRS) \
	libs/alloc \
	libs/data \
	libs/parse

MBR_SRC  := $(wildcard $(MBR_DIR)/*.asm)
ARCH_BOOT_SRC := $(wildcard $(ARCH_DIR)/*.c) $(wildcard $(ARCH_DIR)/*.asm)
ARCH_BOOT_SRC := $(filter-out %64.c %64.asm, $(ARCH_BOOT_SRC))

BOOT_SRC_DIRS := $(filter-out $(ARCH_DIR), $(SRC_DIRS))
BIOS_SRC := $(foreach dir, $(BOOT_SRC_DIRS), $(wildcard $(dir)/*.c) $(wildcard $(dir)/*.asm))
BIOS_SRC += $(ARCH_BOOT_SRC)

MBR_OBJ  := $(patsubst %, bin/boot/%.o, $(MBR_SRC))
BIOS_OBJ := $(patsubst %, bin/boot/%.o, $(BIOS_SRC))

AS_BOOT := -felf32
CC_BOOT := \
	-m32 \
	-fdata-sections \
	-DEXTERNAL_ALLOC \
	-ffunction-sections

bin/boot/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(AS_BOOT), $@, $<)

bin/boot/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(CC_BOOT), $@, $<)


bin/boot/mbr.bin: $(MBR_OBJ)
	@mkdir -p $(@D)
	$(call ld, --oformat=binary -T$(MBR_DIR)/linker.ld, $@, $^)

bin/boot/bios.bin: $(BIOS_OBJ) $(call LIBGCC, $(CC_BOOT))
	@mkdir -p $(@D)
	$(call ld, $(LD_BOOT) -T$(BIOS_DIR)/linker.ld, bin/boot/boot.elf, $^)
	$(call oc, -O binary, bin/boot/boot.elf, $@)
