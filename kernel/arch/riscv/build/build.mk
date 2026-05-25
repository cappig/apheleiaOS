ARCH_DIR        := kernel/arch/riscv
IMAGE_STAGE_DIR := bin/image/$(ARCH)
IMAGE_BOOT_DIR  := $(IMAGE_STAGE_DIR)/boot

USERLAND_TOOLS  ?= all
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

RISCV_UART0   := 0x10000000UL
RISCV_FRISC   ?= false

ifeq ($(RISCV_FRISC),true)
RISCV_UART_STRIDE ?= 4
else
RISCV_UART_STRIDE ?= 1
endif

# The embedded rootfs sits at this byte offset inside the flat image.
# FRISC keeps its DTB in /boot/platform.dtb; the boot stub uses board defaults
# only long enough to read that file from the rootfs.
RISCV_BOOT_IMAGE_ROOTFS_OFFSET := 1441792
RISCV_BOOT_SCRATCH_OFFSET      := 50331648

RISCV_FRISC_DTS        := $(ARCH_DIR)/dts/friscv.dts
RISCV_FRISC_DTB        := $(BOOT_ENTRY_OBJ_DIR)/friscv.dtb
RISCV_PLATFORM_DTB     := /boot/platform.dtb

RISCV_64_ISA_FLAGS := -march=rv64ima_zicsr -mabi=lp64
RISCV_32_ISA_FLAGS := -march=rv32ima_zicsr -mabi=ilp32

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
	-DRISCV_BOOT_SCRATCH_OFFSET=$(RISCV_BOOT_SCRATCH_OFFSET) \
	-DSERIAL_UART0=$(RISCV_UART0) \
	-DRISCV_UART_STRIDE=$(RISCV_UART_STRIDE) \
	-mcmodel=medany

ifeq ($(RISCV_FRISC),true)
BOOT_ENTRY_CFLAGS_COMMON += \
	-DRISCV_FRISC=1 \
	-DRISCV_BOOT_PLATFORM_DTB=\"$(RISCV_PLATFORM_DTB)\"
endif

ifeq ($(RISCV_BOOT_FORCE_NO_DTB), true)
BOOT_ENTRY_CFLAGS_COMMON += -DRISCV_BOOT_FORCE_NO_DTB=1
endif

ifneq ($(RISCV_BOOT_IMAGE_BASE_OVERRIDE),)
BOOT_ENTRY_CFLAGS_COMMON += -DRISCV_BOOT_IMAGE_BASE_OVERRIDE=$(RISCV_BOOT_IMAGE_BASE_OVERRIDE)
endif

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

KERNEL_FLAG_STAMP     := $(KERNEL_OBJ_DIR)/.compile-flags
BOOT_ENTRY_FLAG_STAMP := $(BOOT_ENTRY_OBJ_DIR)/.compile-flags
KERNEL_BUILD_CONFIG   := $(CC) $(CC_BASE) $(KERNEL_CC_FLAGS)
BOOT_ENTRY_CONFIG     := $(CC) $(CC_BASE) $(BOOT_ENTRY_CFLAGS)

$(eval $(call flag_stamp,$(KERNEL_FLAG_STAMP),KERNEL_BUILD_CONFIG))
$(eval $(call flag_stamp,$(BOOT_ENTRY_FLAG_STAMP),BOOT_ENTRY_CONFIG))

$(BOOT_ENTRY_OBJ_DIR)/%.S.o: %.S $(BOOT_ENTRY_FLAG_STAMP)
	@mkdir -p $(@D)
	$(call cc, $(BOOT_ENTRY_CFLAGS), $@, $<)

$(BOOT_ENTRY_OBJ_DIR)/%.c.o: %.c $(BOOT_ENTRY_FLAG_STAMP)
	@mkdir -p $(@D)
	$(call cc, $(BOOT_ENTRY_CFLAGS), $@, $<)

$(BOOT_ENTRY_ELF): $(BOOT_ENTRY_OBJ) $(call LIBGCC, $(BOOT_ENTRY_CFLAGS)) | $(BOOT_ENTRY_LINKER)
	@mkdir -p $(@D)
	@$(CC) $(CC_BASE) $(BOOT_ENTRY_CFLAGS) $(BOOT_ENTRY_LDFLAGS) -o $@ $^
	@printf "%-3s  %s\n" "LD" "$@"

$(BOOT_ENTRY_BIN): $(BOOT_ENTRY_ELF)
	$(call oc, -O binary, $<, $@)

$(KERNEL_OBJ_DIR)/%.S.o: %.S $(KERNEL_FLAG_STAMP)
	@mkdir -p $(@D)
	$(call cc, $(KERNEL_CC_FLAGS), $@, $<)

$(KERNEL_OBJ_DIR)/%.c.o: %.c $(KERNEL_FLAG_STAMP)
	@mkdir -p $(@D)
	$(call cc, $(KERNEL_CC_FLAGS), $@, $<)

$(KERNEL_ELF): $(KERNEL_OBJ) $(call LIBGCC, $(KERNEL_CC_FLAGS)) \
	$(ARCH_DIR)/build/linker$(ARCH_VARIANT).ld
	@mkdir -p $(@D)
	$(call ld, $(KERNEL_LD_FLAGS), $@, $(filter-out $(ARCH_DIR)/build/linker$(ARCH_VARIANT).ld, $^))
	$(call kernel_strip, $@)

IMAGE_SCRIPT_DEPS := \
	utils/stage_image.sh \
	kernel/build_image_common.py \
	kernel/arch/riscv/build/check_boot_stub.py \
	kernel/arch/riscv/build/build_riscv_disk_image.py \
	kernel/arch/riscv/build/check_image_layout.py

IMAGE_ROOT_DEPS := $(shell find root -type f -o -type l)

$(RISCV_FRISC_DTB): $(RISCV_FRISC_DTS)
	@mkdir -p $(@D)
	@command -v dtc >/dev/null 2>&1 || { \
		echo "dtc is required for RISCV_FRISC=true"; \
		exit 1; \
	}
	@dtc -I dts -O dtb -o $@ $<
	@printf "%-3s  %s\n" "DTB" "$@"

ifeq ($(RISCV_FRISC),true)
RISCV_ROOTFS_DTB_DEPS := $(RISCV_FRISC_DTB)
endif

$(ROOTFS_IMAGE): $(BOOT_ENTRY_ELF) $(KERNEL_ELF) $(IMAGE_SCRIPT_DEPS) \
	$(IMAGE_ROOT_DEPS) $(RISCV_ROOTFS_DTB_DEPS)
	@utils/stage_image.sh "$(IMAGE_STAGE_DIR)" "$(IMAGE_BOOT_DIR)" \
		"$(KERNEL_ELF)" "bin/user/$(ARCH)/root" riscv
ifeq ($(RISCV_FRISC),true)
	@cp -f "$(RISCV_FRISC_DTB)" "$(IMAGE_STAGE_DIR)$(RISCV_PLATFORM_DTB)"
	@printf "%-3s  %s\n" "DTB" "$(IMAGE_STAGE_DIR)$(RISCV_PLATFORM_DTB)"
endif
	@python3 kernel/arch/riscv/build/build_riscv_disk_image.py $@ $(IMAGE_STAGE_DIR)
	@printf "%-3s  %s\n" "IM" "$@"

bin/$(IMAGE_NAME).img: $(BOOT_ENTRY_BIN) $(ROOTFS_IMAGE)
	@mkdir -p $(@D)
	@python3 kernel/arch/riscv/build/check_boot_stub.py \
		$(BOOT_ENTRY_ELF) $(BOOT_ENTRY_BIN) $(RISCV_BOOT_IMAGE_ROOTFS_OFFSET)
	@python3 kernel/arch/riscv/build/check_image_layout.py \
	    $(KERNEL_ELF) $(ROOTFS_IMAGE) $(RISCV_BOOT_IMAGE_ROOTFS_OFFSET) \
	    $(RISCV_BOOT_SCRATCH_OFFSET)
	@cp $(BOOT_ENTRY_BIN) $@
	@truncate -s $(RISCV_BOOT_IMAGE_ROOTFS_OFFSET) $@
	@cat $(ROOTFS_IMAGE) >> $@
	@printf "%-3s  %s\n" "IM" "$@"
