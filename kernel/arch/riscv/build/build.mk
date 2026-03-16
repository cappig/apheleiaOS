ARCH_DIR        := kernel/arch/riscv
IMAGE_STAGE_DIR := bin/image/$(ARCH)
IMAGE_BOOT_DIR  := $(IMAGE_STAGE_DIR)/boot

BOOT_ENTRY_DIR     := $(ARCH_DIR)/boot/entry
BOOT_ENTRY_LINKER  := $(BOOT_ENTRY_DIR)/linker.ld
BOOT_ENTRY_SRC     := $(wildcard $(BOOT_ENTRY_DIR)/*.S) $(wildcard $(BOOT_ENTRY_DIR)/*.c)
BOOT_ENTRY_OBJ_DIR := bin/boot
BOOT_ENTRY_OBJ     := $(patsubst %, $(BOOT_ENTRY_OBJ_DIR)/%.o, $(BOOT_ENTRY_SRC))
BOOT_ENTRY_ELF     := bin/boot/riscv_boot.elf
BOOT_ENTRY_BIN     := bin/boot/riscv_boot.bin

BOOT_ENTRY_CFLAGS := \
	-ffreestanding \
	-nostdlib \
	-nostdinc \
	-fno-builtin \
	-fno-pic \
	-fno-pie \
	-mcmodel=medany

ifeq ($(ARCH_VARIANT), 64)
BOOT_ENTRY_CFLAGS += -march=rv64imac -mabi=lp64
else ifeq ($(ARCH_VARIANT), 32)
BOOT_ENTRY_CFLAGS += -march=rv32imac -mabi=ilp32
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)' for ARCH '$(ARCH)')
endif

BOOT_ENTRY_LDFLAGS := \
	-nostdlib \
	-Wl,-T$(BOOT_ENTRY_LINKER) \
	-Wl,--gc-sections

ifneq ($(IMAGE_FORMAT),img)
$(error Unsupported IMAGE_FORMAT '$(IMAGE_FORMAT)' for ARCH '$(ARCH)')
endif

$(BOOT_ENTRY_OBJ_DIR)/%.S.o: %.S
	@mkdir -p $(@D)
	@$(CC) $(BOOT_ENTRY_CFLAGS) -c -o $@ $<
	@echo "CC $<"

$(BOOT_ENTRY_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	@$(CC) $(BOOT_ENTRY_CFLAGS) -c -o $@ $<
	@echo "CC $<"

$(BOOT_ENTRY_ELF): $(BOOT_ENTRY_OBJ)
	@mkdir -p $(@D)
	@$(CC) $(BOOT_ENTRY_CFLAGS) $(BOOT_ENTRY_LDFLAGS) -o $@ $^
	@echo "LD $@"

$(BOOT_ENTRY_BIN): $(BOOT_ENTRY_ELF)
	@mkdir -p $(@D)
	@$(OC) -O binary $< $@
	@echo "OC $@"

IMAGE_SCRIPT_DEPS := \
	kernel/build_image_common.py \
	kernel/arch/riscv/build/build_riscv_disk_image.py

IMAGE_ROOT_DEPS := $(shell find root -type f -o -type l)


define stage_image
	@mkdir -p $(@D)
	@rm -rf $(IMAGE_STAGE_DIR)
	@mkdir -p $(IMAGE_BOOT_DIR)
	@cp -r root/* $(IMAGE_STAGE_DIR)
endef

bin/$(IMAGE_NAME).img: $(BOOT_ENTRY_BIN) $(IMAGE_SCRIPT_DEPS) $(IMAGE_ROOT_DEPS)
	$(call stage_image)
	@python3 kernel/arch/riscv/build/build_riscv_disk_image.py $@ $(BOOT_ENTRY_BIN) $(IMAGE_STAGE_DIR)
