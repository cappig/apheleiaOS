ARCH_DIR        := kernel/arch/riscv
IMAGE_STAGE_DIR := bin/image/$(ARCH)
IMAGE_BOOT_DIR  := $(IMAGE_STAGE_DIR)/boot

USERLAND_TOOLS  ?= none
USERLAND_UI     ?= none
USERLAND_EXTRAS ?= none
USERLAND_GAMES  ?= none

BOOT_ENTRY_DIR    := $(ARCH_DIR)/boot
BOOT_ENTRY_LINKER := $(BOOT_ENTRY_DIR)/linker.ld
ARCH_COMMON_DIR   := kernel/arch/common

KERNEL_SRC_DIRS := \
	kernel \
	$(filter-out kernel/arch, $(wildcard kernel/*)) \
	$(ARCH_DIR) \
	$(filter-out $(ARCH_DIR)/boot $(ARCH_DIR)/build, $(wildcard $(ARCH_DIR)/*)) \
	$(LIBC_DIRS) \
	libs/log \
	libs/alloc \
	libs/data \
	libs/input \
	libs/term \
	libs/parse

KERNEL_ALL_SRC := $(foreach dir, $(KERNEL_SRC_DIRS), \
	$(wildcard $(dir)/*.c) $(wildcard $(dir)/*.S))
KERNEL_ALL_SRC := $(filter-out libs/libc/math.c, $(KERNEL_ALL_SRC))
KERNEL_ALL_SRC := $(filter-out kernel/drivers/registry_%.c, $(KERNEL_ALL_SRC))

include kernel/drivers/build.mk
KERNEL_ALL_SRC := $(sort $(KERNEL_ALL_SRC) $(KERNEL_DRIVER_SRC))

KERNEL_COMMON_SRC := $(filter-out %32.c %64.c %32.S %64.S, $(KERNEL_ALL_SRC))
KERNEL_COMMON_SRC += $(filter %/div64.c, $(KERNEL_ALL_SRC))
KERNEL_ARCH64_SRC := $(filter %64.c %64.S, $(KERNEL_ALL_SRC))
KERNEL_SRC_64     := $(filter-out %/div64.c, $(KERNEL_ARCH64_SRC)) $(KERNEL_COMMON_SRC)
KERNEL_SRC_32     := $(filter %32.c %32.S, $(KERNEL_ALL_SRC)) $(KERNEL_COMMON_SRC)

BOOT_LIBC_SRC := \
	libs/libc/div64.c \
	libs/libc/ctype.c \
	libs/libc/errno.c \
	libs/libc/sprintf.c \
	libs/libc/stdlib.c \
	libs/libc/string.c \
	libs/libc_ext/stdlib.c

BOOT_PARSE_SRC := libs/parse/fdt.c

ARCH_BOOT_SRC := $(ARCH_DIR)/serial.c

BOOT_ENTRY_SRC := \
	$(wildcard $(BOOT_ENTRY_DIR)/*.S) \
	$(wildcard $(BOOT_ENTRY_DIR)/*.c) \
	$(wildcard $(ARCH_COMMON_DIR)/*.c) \
	$(ARCH_BOOT_SRC) \
	$(BOOT_LIBC_SRC) \
	$(BOOT_PARSE_SRC)

BOOT_ENTRY_OBJ_DIR := bin/boot/$(ARCH)
BOOT_ENTRY_OBJ     := $(patsubst %, $(BOOT_ENTRY_OBJ_DIR)/%.o, $(BOOT_ENTRY_SRC))
BOOT_ENTRY_ELF     := $(BOOT_ENTRY_OBJ_DIR)/riscv_boot.elf

KERNEL_CC_COMMON := \
	-I$(ARCH_DIR) \
	-D_KERNEL \
	-DEXTERNAL_ALLOC \
	-fdata-sections \
	-ffunction-sections \
	-fno-omit-frame-pointer

KERNEL_LD_COMMON := --gc-sections

ifeq ($(ARCH_VARIANT), 64)
KERNEL_SRC      := $(KERNEL_SRC_64)
KERNEL_OBJ_DIR  := bin/kernel_riscv64
KERNEL_ELF      := $(KERNEL_OBJ_DIR)/boot/kernel64.elf
KERNEL_CC_FLAGS := $(KERNEL_CC_COMMON) -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany
KERNEL_LD_FLAGS := $(KERNEL_LD_COMMON) -m elf64lriscv -T$(ARCH_DIR)/build/linker64.ld

BOOT_ENTRY_CFLAGS := \
	-Ilibs \
	-Ilibs/libc \
	-Ikernel \
	-Ikernel/arch \
	-I$(ARCH_DIR) \
	-include stdbool.h \
	-ffreestanding \
	-nostdlib \
	-nostdinc \
	-fno-builtin \
	-fno-pic \
	-fno-pie \
	-march=rv64imac_zicsr \
	-mabi=lp64 \
	-mcmodel=medany

BOOT_ENTRY_LDFLAGS := \
	-nostdlib \
	-Wl,-T$(BOOT_ENTRY_LINKER) \
	-Wl,--gc-sections
else ifeq ($(ARCH_VARIANT), 32)
KERNEL_SRC      := $(KERNEL_SRC_32)
KERNEL_OBJ_DIR  := bin/kernel_riscv32
KERNEL_ELF      := $(KERNEL_OBJ_DIR)/boot/kernel32.elf
KERNEL_CC_FLAGS := $(KERNEL_CC_COMMON) -march=rv32imac_zicsr -mabi=ilp32 -mcmodel=medany
KERNEL_LD_FLAGS := $(KERNEL_LD_COMMON) -m elf32lriscv -T$(ARCH_DIR)/build/linker32.ld

BOOT_ENTRY_CFLAGS := \
	-Ilibs \
	-Ilibs/libc \
	-Ikernel \
	-Ikernel/arch \
	-I$(ARCH_DIR) \
	-include stdbool.h \
	-ffreestanding \
	-nostdlib \
	-nostdinc \
	-fno-builtin \
	-fno-pic \
	-fno-pie \
	-march=rv32imac_zicsr \
	-mabi=ilp32 \
	-mcmodel=medany

BOOT_ENTRY_LDFLAGS := \
	-nostdlib \
	-Wl,-T$(BOOT_ENTRY_LINKER) \
	-Wl,--gc-sections
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)')
endif

ifeq ($(TOOLCHAIN), llvm)
BOOT_ENTRY_LDFLAGS += -fuse-ld=lld
endif

KERNEL_OBJ := $(patsubst %, $(KERNEL_OBJ_DIR)/%.o, $(KERNEL_SRC))

$(BOOT_ENTRY_OBJ_DIR)/%.S.o: %.S
	@mkdir -p $(@D)
	@$(CC) $(CC_BASE) $(BOOT_ENTRY_CFLAGS) -c -o $@ $<
	@echo "CC $<"

$(BOOT_ENTRY_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	@$(CC) $(CC_BASE) $(BOOT_ENTRY_CFLAGS) -c -o $@ $<
	@echo "CC $<"

$(BOOT_ENTRY_ELF): $(BOOT_ENTRY_OBJ) | $(BOOT_ENTRY_LINKER)
	@mkdir -p $(@D)
	@$(CC) $(CC_BASE) $(BOOT_ENTRY_CFLAGS) $(BOOT_ENTRY_LDFLAGS) -o $@ $^
	@echo "LD $@"

$(KERNEL_OBJ_DIR)/%.S.o: %.S
	@mkdir -p $(@D)
	$(call cc, $(KERNEL_CC_FLAGS), $@, $<)

$(KERNEL_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(KERNEL_CC_FLAGS), $@, $<)

$(KERNEL_ELF): $(KERNEL_OBJ) $(call LIBGCC, $(KERNEL_CC_FLAGS)) $(ARCH_DIR)/build/linker$(ARCH_VARIANT).ld
	@mkdir -p $(@D)
	$(call ld, $(KERNEL_LD_FLAGS), $@, $(filter-out $(ARCH_DIR)/build/linker$(ARCH_VARIANT).ld, $^))
	@if [ "$(STRIP_KERNEL)" = "true" ]; then \
		$(ST) --strip-debug $@; \
	fi

IMAGE_SCRIPT_DEPS := \
	kernel/build_image_common.py \
	kernel/arch/riscv/build/build_riscv_disk_image.py

IMAGE_ROOT_DEPS := $(shell find root -type f -o -type l)

define stage_image
	@mkdir -p $(@D)
	@rm -rf $(IMAGE_STAGE_DIR)
	@mkdir -p $(IMAGE_BOOT_DIR)
	@cp -f $(KERNEL_ELF) $(IMAGE_BOOT_DIR)/
	@cp -r root/* $(IMAGE_STAGE_DIR)
	@cp -a bin/user/$(ARCH)/root/. $(IMAGE_STAGE_DIR)/
endef

bin/$(IMAGE_NAME).img: $(BOOT_ENTRY_ELF) $(KERNEL_ELF) $(IMAGE_SCRIPT_DEPS) $(IMAGE_ROOT_DEPS)
	$(call stage_image)
	@python3 kernel/arch/riscv/build/build_riscv_disk_image.py $@ $(IMAGE_STAGE_DIR)
