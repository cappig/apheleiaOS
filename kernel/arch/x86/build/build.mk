ARCH_DIR        := kernel/arch/x86
IMAGE_STAGE_DIR := bin/image/$(ARCH_VARIANT)
IMAGE_BOOT_DIR  := $(IMAGE_STAGE_DIR)/boot


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
	libs/term \
	libs/parse

KERNEL_ALL_SRC := $(foreach dir, $(KERNEL_SRC_DIRS), \
	$(wildcard $(dir)/*.c) $(wildcard $(dir)/*.asm))
KERNEL_ALL_SRC := $(filter-out libs/libc/math.c, $(KERNEL_ALL_SRC))
KERNEL_ALL_SRC := $(filter-out kernel/drivers/registry_%.c, $(KERNEL_ALL_SRC))

include kernel/drivers/build.mk
KERNEL_ALL_SRC := $(sort $(KERNEL_ALL_SRC) $(KERNEL_DRIVER_SRC))

KERNEL_COMMON_SRC := $(filter-out %32.c %64.c %32.asm %64.asm, $(KERNEL_ALL_SRC))
KERNEL_COMMON_SRC += $(filter %/div64.c, $(KERNEL_ALL_SRC))
KERNEL_ARCH64_SRC := $(filter %64.c %64.asm, $(KERNEL_ALL_SRC))
KERNEL_SRC_64     := $(filter-out %/div64.c, $(KERNEL_ARCH64_SRC)) $(KERNEL_COMMON_SRC)
KERNEL_SRC_32     := $(filter %32.c %32.asm, $(KERNEL_ALL_SRC)) $(KERNEL_COMMON_SRC)

include kernel/arch/x86/boot/bios/build.mk
include kernel/arch/x86/boot/uefi/build.mk

KERNEL_CC_COMMON := \
	-I$(ARCH_DIR) \
	-D_KERNEL \
	-DEXTERNAL_ALLOC \
	-fdata-sections \
	-ffunction-sections

KERNEL_X86_FP_FLAGS := \
	-mno-mmx \
	-mno-sse \
	-mno-sse2

KERNEL_LD_COMMON := \
	--gc-sections

ifeq ($(ARCH_VARIANT), 64)
KERNEL_SRC      := $(KERNEL_SRC_64)
KERNEL_OBJ_DIR  := bin/kernel64
KERNEL_ELF      := bin/kernel64/boot/kernel64.elf
KERNEL_AS_FLAGS := -felf64
KERNEL_CC_FLAGS := $(KERNEL_CC_COMMON) $(KERNEL_X86_FP_FLAGS) -march=x86-64 -mcmodel=kernel -m64
KERNEL_LD_FLAGS := $(KERNEL_LD_COMMON) -T$(ARCH_DIR)/build/linker64.ld
else ifeq ($(ARCH_VARIANT), 32)
KERNEL_SRC      := $(KERNEL_SRC_32)
KERNEL_OBJ_DIR  := bin/kernel32
KERNEL_ELF      := bin/kernel32/boot/kernel32.elf
KERNEL_AS_FLAGS := -felf32
KERNEL_CC_FLAGS := $(KERNEL_CC_COMMON) $(KERNEL_X86_FP_FLAGS) -m32
KERNEL_LD_FLAGS := $(KERNEL_LD_COMMON) -T$(ARCH_DIR)/build/linker32.ld
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
	@if [ "$(STRIP_KERNEL)" = "true" ]; then \
		$(ST) --strip-debug $@; \
	fi


IMAGE_BOOT_DEPS := bin/boot/bios.bin bin/boot/mbr.bin $(KERNEL_ELF)
ifeq ($(ARCH_VARIANT), 64)
IMAGE_BOOT_DEPS += bin/boot/BOOTX64.EFI
endif

IMAGE_UEFI_BOOTLOADER :=
IMAGE_UEFI_KERNEL :=
ifeq ($(ARCH_VARIANT), 64)
IMAGE_UEFI_BOOTLOADER := bin/boot/BOOTX64.EFI
IMAGE_UEFI_KERNEL := $(KERNEL_ELF)
endif

IMAGE_SCRIPT_DEPS := \
	kernel/build_image_common.py \
	kernel/arch/x86/build/build_bios_disk_image.py \
	kernel/arch/x86/build/build_hybrid_disk_image.py \
	kernel/arch/x86/build/build_hybrid_iso_image.py

IMAGE_ROOT_DEPS := $(shell find root -type f -o -type l)


define stage_image
	@mkdir -p $(@D)
	@rm -rf $(IMAGE_STAGE_DIR)
	@mkdir -p $(IMAGE_BOOT_DIR)
	@cp -f $(KERNEL_ELF) $(IMAGE_BOOT_DIR)/
	@cp -r root/* $(IMAGE_STAGE_DIR)
	@cp -a bin/user/$(ARCH_VARIANT)/root/. $(IMAGE_STAGE_DIR)/
endef

bin/$(IMAGE_NAME).img: $(IMAGE_BOOT_DEPS) $(IMAGE_SCRIPT_DEPS) $(IMAGE_ROOT_DEPS)
	$(call stage_image)
ifeq ($(ARCH_VARIANT), 64)
	@python3 kernel/arch/x86/build/build_hybrid_disk_image.py $@ \
		bin/boot/mbr.bin \
		bin/boot/bios.bin \
		bin/boot/BOOTX64.EFI \
		$(KERNEL_ELF) \
		$(IMAGE_STAGE_DIR)
else
	@python3 kernel/arch/x86/build/build_bios_disk_image.py $@ bin/boot/mbr.bin bin/boot/bios.bin $(IMAGE_STAGE_DIR)
endif

bin/$(IMAGE_NAME).iso: $(IMAGE_BOOT_DEPS) $(IMAGE_SCRIPT_DEPS) $(IMAGE_ROOT_DEPS)
	$(call stage_image)
	@python3 kernel/arch/x86/build/build_hybrid_iso_image.py $@ \
		bin/boot/mbr.bin \
		bin/boot/bios.bin \
		"$(IMAGE_UEFI_BOOTLOADER)" \
		"$(IMAGE_UEFI_KERNEL)" \
		$(IMAGE_STAGE_DIR)
