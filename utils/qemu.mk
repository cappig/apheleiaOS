QEMU_ARCH := $(ARCH)

ifeq ($(ARCH), x86_32)
QEMU_ARCH := i386
else ifeq ($(ARCH), riscv_64)
QEMU_ARCH := riscv64
endif

QEMU := qemu-system-$(QEMU_ARCH)

QEMU_CONSOLE ?= false
BOOT ?= bios
QEMU_MEMORY ?= 64M

OVMF_DIR := .cache/ovmf
OVMF_CODE_LOCAL := $(OVMF_DIR)/OVMF_CODE.fd
OVMF_VARS_LOCAL := $(OVMF_DIR)/OVMF_VARS.fd
OVMF_FETCH_SCRIPT := utils/ovmf-fetch.sh

# Stable OVMF package from Debian 12
OVMF_DEB_URL ?= https://deb.debian.org/debian/pool/main/e/edk2/ovmf_2022.11-6+deb12u2_all.deb

OVMF_CODE ?= $(OVMF_CODE_LOCAL)
OVMF_VARS ?= $(OVMF_VARS_LOCAL)
OVMF_VARS_RUNTIME := bin/ovmf_vars.fd

QEMU_CONSOLE_ARGS :=
ifeq ($(QEMU_CONSOLE), true)
QEMU_CONSOLE_ARGS := \
	-s \
	-monitor stdio \
	-d int,cpu_reset,guest_errors,mmu
else
QEMU_CONSOLE_ARGS := \
	-serial stdio
endif

QEMU_ARGS := \
	-no-reboot \
	-m $(QEMU_MEMORY) \
	$(QEMU_CONSOLE_ARGS)

.PHONY: ovmf-fetch
ovmf-fetch:
	@$(OVMF_FETCH_SCRIPT) "$(OVMF_DIR)" "$(OVMF_DEB_URL)"

.PHONY: ovmf-clean
ovmf-clean:
	@rm -rf "$(OVMF_DIR)"

# 	@test -f "$(OVMF_CODE)" || (echo "Missing OVMF code firmware: $(OVMF_CODE)" && false)
# 	@test -f "$(OVMF_VARS)" || (echo "Missing OVMF vars template: $(OVMF_VARS)" && false)
.PHONY: run
run: bin/$(IMG_NAME)
ifeq ($(BOOT), uefi)
ifeq ($(ARCH), x86_64)
ifeq ($(OVMF_CODE), $(OVMF_CODE_LOCAL))
ifeq ($(OVMF_VARS), $(OVMF_VARS_LOCAL))
run: ovmf-fetch
endif
endif
	@mkdir -p $(@D)
	@cp -f "$(OVMF_VARS)" "$(OVMF_VARS_RUNTIME)"
	@$(QEMU) $(QEMU_ARGS) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS_RUNTIME)" \
		-drive format=raw,file=bin/$(IMG_NAME),if=ide,index=0 \
		-drive if=none,id=uefi_esp,file=fat:rw:$(UEFI_STAGE_DIR),format=raw \
		-device ide-hd,drive=uefi_esp,bus=ide.0,unit=1,bootindex=0
else
	$(error BOOT=uefi is only supported with ARCH=x86_64)
endif
else
	@$(QEMU) $(QEMU_ARGS) \
		-drive format=raw,file=bin/$(IMG_NAME)
endif
