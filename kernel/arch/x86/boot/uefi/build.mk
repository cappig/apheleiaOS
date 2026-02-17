UEFI_MAKE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

UEFI_DIR := $(UEFI_MAKE_DIR)
UEFI_STAGE_DIR := bin/uefi

UEFI_SRC := \
	$(UEFI_DIR)/main.c \
	$(UEFI_DIR)/util.c

UEFI_OBJ := $(patsubst $(UEFI_DIR)/%.c,bin/uefi/obj/%.o,$(UEFI_SRC))

ifeq ($(TOOLCHAIN), llvm)
UEFI_CC := $(CC)
UEFI_LD := $(LD)
UEFI_CFLAGS := \
	-target x86_64-unknown-uefi \
	-fshort-wchar \
	-fdata-sections \
	-ffunction-sections
UEFI_LDFLAGS := \
	-m i386pep \
	--subsystem efi_application \
	--entry efi_main \
	--gc-sections
else ifeq ($(TOOLCHAIN), gnu)
UEFI_CC := x86_64-w64-mingw32-gcc
UEFI_LD := x86_64-w64-mingw32-ld
UEFI_CFLAGS := \
	-fshort-wchar \
	-fdata-sections \
	-ffunction-sections
UEFI_LDFLAGS := \
	-m i386pep \
	--subsystem 10 \
	--entry efi_main \
	--enable-reloc-section \
	--disable-high-entropy-va \
	--gc-sections
else
$(error UEFI build does not support TOOLCHAIN='$(TOOLCHAIN)')
endif

bin/uefi/obj/%.o: CC := $(UEFI_CC)
bin/uefi/obj/%.o: $(UEFI_DIR)/%.c $(UEFI_DIR)/efi.h $(UEFI_DIR)/util.h
	@mkdir -p $(@D)
	$(call cc, $(UEFI_CFLAGS), $@, $<)

bin/boot/BOOTX64.EFI: LD := $(UEFI_LD)
bin/boot/BOOTX64.EFI: LD_BASE :=
bin/boot/BOOTX64.EFI: $(UEFI_OBJ)
	@mkdir -p $(@D)
	$(call ld, $(UEFI_LDFLAGS), $@, $^)
	@echo "UEFI $@"

ifeq ($(ARCH_VARIANT), 64)
bin/uefi/EFI/BOOT/BOOTX64.EFI: bin/boot/BOOTX64.EFI
	@mkdir -p $(@D)
	@cp -f $< $@

bin/uefi/boot/kernel64.elf: bin/kernel64/boot/kernel64.elf
	@mkdir -p $(@D)
	@cp -f $< $@
endif
