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

BOOT_ENTRY_SRC := \
	$(wildcard $(BOOT_ENTRY_DIR)/*.S) \
	$(wildcard $(BOOT_ENTRY_DIR)/*.c) \
	$(wildcard $(ARCH_COMMON_DIR)/*.c) \
	$(ARCH_DIR)/serial.c \
	libs/libc/builtins.c \
	libs/libc/div64.c \
	libs/libc/ctype.c \
	libs/libc/errno.c \
	libs/libc/sprintf.c \
	libs/libc/stdlib.c \
	libs/libc/string.c \
	libs/libc_ext/stdlib.c \
	libs/parse/fdt.c

BOOT_ENTRY_OBJ_DIR := bin/boot/$(ARCH)
BOOT_ENTRY_OBJ     := $(patsubst %, $(BOOT_ENTRY_OBJ_DIR)/%.o, $(BOOT_ENTRY_SRC))
BOOT_ENTRY_ELF     := $(BOOT_ENTRY_OBJ_DIR)/riscv_boot.elf
BOOT_ENTRY_BIN     := $(BOOT_ENTRY_OBJ_DIR)/riscv_boot.bin
ROOTFS_IMAGE       := bin/$(IMAGE_NAME).rootfs.img

# The embedded rootfs sits at this byte offset inside the flat image.
# The boot stub must fit below it; the kernel loads above it.
RISCV_BOOT_IMAGE_ROOTFS_OFFSET := 2097152

RISCV_UART0        := 0x10000000UL
RISCV_UART_STRIDE  ?= 1

RISCV_64_ISA_FLAGS := -march=rv64ia_zicsr -mabi=lp64
RISCV_32_ISA_FLAGS := -march=rv32ia_zicsr -mabi=ilp32

KERNEL_CC_COMMON := \
	-I$(ARCH_DIR) \
	-D_KERNEL \
	-DEXTERNAL_ALLOC \
	-DSERIAL_UART0=$(RISCV_UART0) \
	-DRISCV_UART_STRIDE=$(RISCV_UART_STRIDE) \
	-fdata-sections \
	-ffunction-sections \
	-fno-omit-frame-pointer

KERNEL_LD_COMMON := --gc-sections

# Flags shared by both the 64-bit and 32-bit boot stub builds.
# The arch-specific ISA flags are appended per-variant below.
BOOT_ENTRY_CFLAGS_COMMON := \
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
	-DRISCV_BOOT_IMAGE_ROOTFS_OFFSET=$(RISCV_BOOT_IMAGE_ROOTFS_OFFSET) \
	-DSERIAL_UART0=$(RISCV_UART0) \
	-DRISCV_UART_STRIDE=$(RISCV_UART_STRIDE) \
	-mcmodel=medany

BOOT_ENTRY_LDFLAGS := \
	-nostdlib \
	-Wl,-T$(BOOT_ENTRY_LINKER) \
	-Wl,--gc-sections

ifeq ($(ARCH_VARIANT), 64)
KERNEL_SRC      := $(KERNEL_SRC_64)
KERNEL_OBJ_DIR  := bin/kernel_riscv64
KERNEL_ELF      := $(KERNEL_OBJ_DIR)/boot/kernel64.elf
KERNEL_CC_FLAGS := $(KERNEL_CC_COMMON) $(RISCV_64_ISA_FLAGS) -mcmodel=medany
KERNEL_LD_FLAGS := $(KERNEL_LD_COMMON) -m elf64lriscv -T$(ARCH_DIR)/build/linker64.ld
BOOT_ENTRY_CFLAGS := $(BOOT_ENTRY_CFLAGS_COMMON) $(RISCV_64_ISA_FLAGS)
else ifeq ($(ARCH_VARIANT), 32)
KERNEL_SRC      := $(KERNEL_SRC_32)
KERNEL_OBJ_DIR  := bin/kernel_riscv32
KERNEL_ELF      := $(KERNEL_OBJ_DIR)/boot/kernel32.elf
KERNEL_CC_FLAGS := $(KERNEL_CC_COMMON) $(RISCV_32_ISA_FLAGS) -mcmodel=medany
KERNEL_LD_FLAGS := $(KERNEL_LD_COMMON) -m elf32lriscv -T$(ARCH_DIR)/build/linker32.ld
BOOT_ENTRY_CFLAGS := $(BOOT_ENTRY_CFLAGS_COMMON) $(RISCV_32_ISA_FLAGS)
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)')
endif

ifeq ($(TOOLCHAIN), llvm)
BOOT_ENTRY_LDFLAGS += -fuse-ld=lld
ifeq ($(ARCH_VARIANT), 32)
KERNEL_CC_FLAGS += -Wno-atomic-alignment
endif
endif

KERNEL_OBJ := $(patsubst %, $(KERNEL_OBJ_DIR)/%.o, $(KERNEL_SRC))

$(BOOT_ENTRY_OBJ_DIR)/%.S.o: %.S
	@mkdir -p $(@D)
	@$(CC) $(CC_BASE) $(BOOT_ENTRY_CFLAGS) -c -o $@ $<
	@printf "%s  %s\n" "CC" "$<"

$(BOOT_ENTRY_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	@$(CC) $(CC_BASE) $(BOOT_ENTRY_CFLAGS) -c -o $@ $<
	@printf "%s  %s\n" "CC" "$<"

$(BOOT_ENTRY_ELF): $(BOOT_ENTRY_OBJ) $(call LIBGCC, $(BOOT_ENTRY_CFLAGS)) | $(BOOT_ENTRY_LINKER)
	@mkdir -p $(@D)
	@$(CC) $(CC_BASE) $(BOOT_ENTRY_CFLAGS) $(BOOT_ENTRY_LDFLAGS) -o $@ $^
	@printf "%s  %s\n" "LD" "$@"

$(BOOT_ENTRY_BIN): $(BOOT_ENTRY_ELF)
	$(call oc, -O binary, $<, $@)

$(KERNEL_OBJ_DIR)/%.S.o: %.S
	@mkdir -p $(@D)
	$(call cc, $(KERNEL_CC_FLAGS), $@, $<)

$(KERNEL_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(KERNEL_CC_FLAGS), $@, $<)

$(KERNEL_ELF): $(KERNEL_OBJ) $(call LIBGCC, $(KERNEL_CC_FLAGS)) $(ARCH_DIR)/build/linker$(ARCH_VARIANT).ld
	@mkdir -p $(@D)
	$(call ld, $(KERNEL_LD_FLAGS), $@, $(filter-out $(ARCH_DIR)/build/linker$(ARCH_VARIANT).ld, $^))
	@if [ "$(STRIP_KERNEL)" = "true" ]; then $(ST) --strip-debug $@; fi

IMAGE_SCRIPT_DEPS := \
	kernel/build_image_common.py \
	kernel/arch/riscv/build/build_riscv_disk_image.py \
	kernel/arch/riscv/build/check_image_layout.py

IMAGE_ROOT_DEPS := $(shell find root -type f -o -type l)

define stage_image
	@mkdir -p $(@D)
	@rm -rf $(IMAGE_STAGE_DIR)
	@mkdir -p $(IMAGE_BOOT_DIR)
	@cp -f $(KERNEL_ELF) $(IMAGE_BOOT_DIR)/
	@cp -r root/* $(IMAGE_STAGE_DIR)
	@cp -a bin/user/$(ARCH)/root/. $(IMAGE_STAGE_DIR)/
	@rm -rf $(IMAGE_STAGE_DIR)/usr/lib
	@rm -rf $(IMAGE_STAGE_DIR)/etc/cursors
	@rm -f  $(IMAGE_STAGE_DIR)/home/user/wall.ppm
endef

$(ROOTFS_IMAGE): $(BOOT_ENTRY_ELF) $(KERNEL_ELF) $(IMAGE_SCRIPT_DEPS) $(IMAGE_ROOT_DEPS)
	$(call stage_image)
	@python3 kernel/arch/riscv/build/build_riscv_disk_image.py $@ $(IMAGE_STAGE_DIR)
	@printf "%s  %s\n" "IM" "$@"

bin/$(IMAGE_NAME).img: $(BOOT_ENTRY_BIN) $(ROOTFS_IMAGE)
	@mkdir -p $(@D)
	@stack_top=$$(printf '%d' 0x$$(readelf -Ws $(BOOT_ENTRY_ELF) \
	    | awk '$$NF == "__stack_top" { print $$2; exit }')); \
	footprint=$$((stack_top - 0x80000000)); \
	[ "$$footprint" -le "$(RISCV_BOOT_IMAGE_ROOTFS_OFFSET)" ] || \
	    { echo "boot image footprint exceeds embedded rootfs offset"; exit 1; }
	@[ "$$(wc -c < $(BOOT_ENTRY_BIN))" -le "$(RISCV_BOOT_IMAGE_ROOTFS_OFFSET)" ] || \
	    { echo "boot binary exceeds embedded rootfs offset"; exit 1; }
	@python3 kernel/arch/riscv/build/check_image_layout.py \
	    $(KERNEL_ELF) $(ROOTFS_IMAGE) $(RISCV_BOOT_IMAGE_ROOTFS_OFFSET)
	@cp $(BOOT_ENTRY_BIN) $@
	@truncate -s $(RISCV_BOOT_IMAGE_ROOTFS_OFFSET) $@
	@cat $(ROOTFS_IMAGE) >> $@
	@printf "%s  %s\n" "IM" "$@"
