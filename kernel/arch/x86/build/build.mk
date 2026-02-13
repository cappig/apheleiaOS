ARCH_DIR := kernel/arch/x86
IMAGE_STAGE_DIR := bin/image
IMAGE_BOOT_DIR := $(IMAGE_STAGE_DIR)/boot
IMAGE_SBIN_DIR := $(IMAGE_STAGE_DIR)/sbin

KERNEL_SRC_DIRS := \
	kernel \
	$(filter-out kernel/arch, $(wildcard kernel/*)) \
	$(ARCH_DIR) \
	$(filter-out $(ARCH_DIR)/boot, $(wildcard $(ARCH_DIR)/*)) \
	$(LIBC_DIRS) \
	libs/log \
	libs/alloc \
	libs/data \
	libs/input \
	libs/parse

KERNEL_ALL_SRC := $(foreach dir, $(KERNEL_SRC_DIRS), $(wildcard $(dir)/*.c) $(wildcard $(dir)/*.asm))

KERNEL_COMMON_SRC := $(filter-out %32.c %64.c %32.asm %64.asm, $(KERNEL_ALL_SRC))
KERNEL_SRC_64 := $(filter %64.c %64.asm, $(KERNEL_ALL_SRC)) $(KERNEL_COMMON_SRC)
KERNEL_SRC_32 := $(filter %32.c %32.asm, $(KERNEL_ALL_SRC)) $(KERNEL_COMMON_SRC)

include kernel/arch/x86/boot/bios/build.mk
include kernel/arch/x86/boot/uefi/build.mk

ifeq ($(BOOT), uefi)
ifeq ($(ARCH_VARIANT), 32)
$(error BOOT=uefi is only supported with ARCH=x86_64)
endif
endif

ifeq ($(ARCH_VARIANT), 64)
KERNEL_SRC := $(KERNEL_SRC_64)
KERNEL_OBJ_DIR := bin/kernel64
KERNEL_ELF := bin/kernel64/boot/kernel64.elf
KERNEL_AS_FLAGS := -felf64
KERNEL_CC_FLAGS := \
	-fdata-sections \
	-D_KERNEL \
	-DEXTERNAL_ALLOC \
	-ffunction-sections \
	-march=x86-64 \
	-mcmodel=kernel \
	-m64
KERNEL_LD_FLAGS := \
	--gc-sections \
	-T$(ARCH_DIR)/build/linker64.ld
else ifeq ($(ARCH_VARIANT), 32)
KERNEL_SRC := $(KERNEL_SRC_32)
KERNEL_OBJ_DIR := bin/kernel32
KERNEL_ELF := bin/kernel32/boot/kernel32.elf
KERNEL_AS_FLAGS := -felf32
KERNEL_CC_FLAGS := \
	-fdata-sections \
	-D_KERNEL \
	-DEXTERNAL_ALLOC \
	-ffunction-sections \
	-m32
KERNEL_LD_FLAGS := \
	--gc-sections \
	-T$(ARCH_DIR)/build/linker32.ld
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)')
endif

KERNEL_OBJ := $(patsubst %, $(KERNEL_OBJ_DIR)/%.o, $(KERNEL_SRC))

$(KERNEL_OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(KERNEL_AS_FLAGS), $@, $<)

$(KERNEL_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(KERNEL_CC_FLAGS), $@, $<)

$(KERNEL_ELF): $(KERNEL_OBJ) $(call LIBGCC, $(KERNEL_CC_FLAGS))
	@mkdir -p $(@D)
	$(call ld, $(KERNEL_LD_FLAGS), $@, $^)

SYMBOL_MAP := $(dir $(KERNEL_ELF))sym.map

$(SYMBOL_MAP): $(KERNEL_ELF)
	@mkdir -p $(@D)
	@if [ "$(TRACEABLE_KERNEL)" = "true" ]; then \
		$(NM) -n $< > $@; \
		echo "NM $@"; \
	else \
		rm -f $@; \
		touch $@; \
	fi

bin/$(IMG_NAME): bin/boot/bios.bin bin/boot/mbr.bin $(KERNEL_ELF) $(SYMBOL_MAP)
	@mkdir -p $(@D)
	@rm -rf $(IMAGE_STAGE_DIR)
	@mkdir -p $(IMAGE_BOOT_DIR)
	@cp -f $(KERNEL_ELF) $(IMAGE_BOOT_DIR)/
	@cp -f $(SYMBOL_MAP) $(IMAGE_BOOT_DIR)/sym.map
	@cp -r root/* $(IMAGE_STAGE_DIR)
	@mkdir -p $(IMAGE_SBIN_DIR)
	@cp -f bin/user/$(ARCH_VARIANT)/root/sbin/* $(IMAGE_SBIN_DIR)/
	@kernel/image.sh $@ $< $(IMAGE_STAGE_DIR)
	@kernel/arch/x86/build/mbr.sh bin/boot/mbr.bin $@

bin/$(BUILD_NAME)_$(ARCH).iso: bin/$(IMG_NAME)
	@mkdir -p $(@D)
	@cp -f $< $@
	@rm -f bin/$(BUILD_NAME)_$(ARCH)
	@rm -f $<
	@echo "ISO $@"

ifeq ($(BOOT), uefi)
ifeq ($(ARCH_VARIANT), 64)
run: bin/uefi/EFI/BOOT/BOOTX64.EFI bin/uefi/boot/kernel64.elf
endif
endif
