BIOS_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
MBR_DIR  := $(dir $(BIOS_DIR))mbr
BOOT_LIB_DIRS := $(ARCH_DIR) kernel/lib kernel/arch/common


BIOS_SRC_DIRS := \
	$(BIOS_DIR) \
	$(BOOT_LIB_DIRS) \
	$(LIBC_DIRS) \
	libs/alloc \
	libs/data \
	libs/parse

ARCH_BOOT_SRC := \
	$(ARCH_DIR)/e820.c \
	$(ARCH_DIR)/serial.c

BIOS_SCAN_DIRS := $(filter-out $(ARCH_DIR), $(BIOS_SRC_DIRS))

BIOS_SRC := \
	$(foreach dir, $(BIOS_SCAN_DIRS), $(wildcard $(dir)/*.c) $(wildcard $(dir)/*.asm)) \
	$(ARCH_BOOT_SRC)

MBR_SRC := $(wildcard $(MBR_DIR)/*.asm)

MBR_OBJ  := $(patsubst %, bin/boot/%.o, $(MBR_SRC))
BIOS_OBJ := $(patsubst %, bin/boot/%.o, $(BIOS_SRC))

AS_BOOT := -felf32
BOOT_X86_FP_FLAGS := \
	-mno-mmx \
	-mno-sse \
	-mno-sse2

CC_BOOT := \
	-m32 \
	$(BOOT_X86_FP_FLAGS) \
	-fdata-sections \
	-DEXTERNAL_ALLOC \
	-ffunction-sections

LD_MBR := \
	--oformat=binary \
	-T$(MBR_DIR)/linker.ld

LD_BIOS := \
	-T$(BIOS_DIR)/linker.ld

OC_BIOS := -O binary


bin/boot/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(AS_BOOT), $@, $<)

bin/boot/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(CC_BOOT), $@, $<)

bin/boot/mbr.bin: $(MBR_OBJ)
	@mkdir -p $(@D)
	$(call ld, $(LD_MBR), $@, $^)

bin/boot/bios.bin: $(BIOS_OBJ) $(call LIBGCC, $(CC_BOOT))
	@mkdir -p $(@D)
	$(call ld, $(LD_BIOS), bin/boot/boot.elf, $^)
	$(call oc, $(OC_BIOS), bin/boot/boot.elf, $@)
