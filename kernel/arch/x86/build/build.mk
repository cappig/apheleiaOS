ARCH_DIR        := kernel/arch/x86
IMAGE_STAGE_DIR := bin/image/$(ARCH_VARIANT)
IMAGE_BOOT_DIR  := $(IMAGE_STAGE_DIR)/boot

X86_NO_FP_FLAGS := \
	-mno-mmx \
	-mno-sse \
	-mno-sse2

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

CC_BASE += -mno-red-zone

KERNEL_CC_COMMON := \
	-I$(ARCH_DIR) \
	-D_KERNEL \
	-DEXTERNAL_ALLOC \
	-fdata-sections \
	-ffunction-sections

KERNEL_LD_COMMON := \
	--gc-sections

ifeq ($(ARCH_VARIANT), 64)
KERNEL_SRC      := $(KERNEL_SRC_64)
KERNEL_OBJ_DIR  := bin/kernel64
KERNEL_ELF      := bin/kernel64/boot/kernel64.elf
KERNEL_AS_FLAGS := -felf64
KERNEL_CC_FLAGS := \
	$(KERNEL_CC_COMMON) \
	$(X86_NO_FP_FLAGS) \
	-march=x86-64 \
	-mcmodel=kernel \
	-m64
KERNEL_LD_FLAGS := $(KERNEL_LD_COMMON) -T$(ARCH_DIR)/build/linker64.ld
else ifeq ($(ARCH_VARIANT), 32)
KERNEL_SRC      := $(KERNEL_SRC_32)
KERNEL_OBJ_DIR  := bin/kernel32
KERNEL_ELF      := bin/kernel32/boot/kernel32.elf
KERNEL_AS_FLAGS := -felf32
KERNEL_CC_FLAGS := \
	$(KERNEL_CC_COMMON) \
	$(X86_NO_FP_FLAGS) \
	-m32
KERNEL_LD_FLAGS := $(KERNEL_LD_COMMON) -T$(ARCH_DIR)/build/linker32.ld
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)')
endif

KERNEL_OBJ := $(patsubst %, $(KERNEL_OBJ_DIR)/%.o, $(KERNEL_SRC))
KERNEL_FLAG_STAMP   := $(KERNEL_OBJ_DIR)/.compile-flags
KERNEL_BUILD_CONFIG := $(CC) $(AS) $(CC_BASE) $(AS_BASE) \
	$(KERNEL_CC_FLAGS) $(KERNEL_AS_FLAGS) $(TOOLCHAIN_CONFIG)

$(eval $(call flag_stamp,$(KERNEL_FLAG_STAMP),KERNEL_BUILD_CONFIG))

$(KERNEL_OBJ_DIR)/%.asm.o: %.asm $(KERNEL_FLAG_STAMP)
	@mkdir -p $(@D)
	$(call as, $(KERNEL_AS_FLAGS), $@, $<)

$(KERNEL_OBJ_DIR)/%.c.o: %.c $(KERNEL_FLAG_STAMP)
	@mkdir -p $(@D)
	$(call cc, $(KERNEL_CC_FLAGS), $@, $<)

$(KERNEL_ELF): $(KERNEL_OBJ) $(call LIBGCC, $(KERNEL_CC_FLAGS))
	@mkdir -p $(@D)
	$(call ld, $(KERNEL_LD_FLAGS), $@, $^)
	$(call kernel_strip, $@)


IMAGE_BOOT_DEPS := bin/boot/bios.bin bin/boot/mbr.bin $(KERNEL_ELF)

IMAGE_SCRIPT_DEPS := \
	utils/stage_image.sh \
	kernel/build_image_common.py \
	kernel/arch/x86/build/build_bios_disk_image.py \
	kernel/arch/x86/build/build_bios_iso_image.py

IMAGE_ROOT_DEPS := $(shell find root -type f -o -type l)

bin/$(IMAGE_NAME).img: $(IMAGE_BOOT_DEPS) $(IMAGE_SCRIPT_DEPS) $(IMAGE_ROOT_DEPS)
	@utils/stage_image.sh "$(IMAGE_STAGE_DIR)" "$(IMAGE_BOOT_DIR)" \
		"$(KERNEL_ELF)" "bin/user/$(ARCH)/root"
	@python3 kernel/arch/x86/build/build_bios_disk_image.py $@ \
		bin/boot/mbr.bin bin/boot/bios.bin $(IMAGE_STAGE_DIR)

bin/$(IMAGE_NAME).iso: $(IMAGE_BOOT_DEPS) $(IMAGE_SCRIPT_DEPS) $(IMAGE_ROOT_DEPS)
	@utils/stage_image.sh "$(IMAGE_STAGE_DIR)" "$(IMAGE_BOOT_DIR)" \
		"$(KERNEL_ELF)" "bin/user/$(ARCH)/root"
	@python3 kernel/arch/x86/build/build_bios_iso_image.py $@ \
		bin/boot/mbr.bin \
		bin/boot/bios.bin \
		$(IMAGE_STAGE_DIR)
