QEMU_ARCH := $(ARCH)

ifeq ($(ARCH), x86_32)
QEMU_ARCH := i386
else ifeq ($(ARCH), riscv_64)
QEMU_ARCH := riscv64
else ifeq ($(ARCH), riscv_32)
QEMU_ARCH := riscv32
endif

QEMU := qemu-system-$(QEMU_ARCH)

QEMU_CONSOLE ?= false
BOOT         ?= bios
QEMU_MEMORY  ?= 256M
QEMU_CPU     ?= max
QEMU_SMP     ?= 1
KVM          ?= false
QEMU_SNAPSHOT ?= false
QEMU_MACHINE ?=

ifeq ($(ARCH), riscv_64)
QEMU_MACHINE := virt
endif
ifeq ($(ARCH), riscv_32)
QEMU_MACHINE := virt
endif

ifeq ($(KVM), true)
ifeq ($(QEMU_CPU), max)
QEMU_CPU = host
endif
endif

OVMF_DIR          := .cache/ovmf
OVMF_CODE_LOCAL   := $(OVMF_DIR)/OVMF_CODE.fd
OVMF_VARS_LOCAL   := $(OVMF_DIR)/OVMF_VARS.fd
OVMF_FETCH_SCRIPT := utils/ovmf_fetch.py

OVMF_DEB_URL ?= https://deb.debian.org/debian/pool/main/e/edk2/ovmf_2022.11-6+deb12u2_all.deb

OVMF_CODE         ?= $(OVMF_CODE_LOCAL)
OVMF_VARS         ?= $(OVMF_VARS_LOCAL)
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
	-cpu $(QEMU_CPU) \
	-m $(QEMU_MEMORY) \
	-smp $(QEMU_SMP) \
	$(QEMU_CONSOLE_ARGS)

ifeq ($(KVM), true)
QEMU_ARGS += -enable-kvm
endif

ifneq ($(QEMU_MACHINE),)
QEMU_ARGS += -machine $(QEMU_MACHINE)
endif

ifeq ($(QEMU_SNAPSHOT), true)
QEMU_ARGS += -snapshot
endif

QEMU_BOOT_DEPS :=
QEMU_BOOT_SETUP := @:
QEMU_BOOT_ARGS :=

ifeq ($(BOOT), uefi)
ifeq ($(ARCH), x86_64)
QEMU_BOOT_SETUP := @cp -f "$(OVMF_VARS)" "$(OVMF_VARS_RUNTIME)"
QEMU_BOOT_ARGS := \
	-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
	-drive if=pflash,format=raw,file="$(OVMF_VARS_RUNTIME)"
ifeq ($(OVMF_CODE), $(OVMF_CODE_LOCAL))
ifeq ($(OVMF_VARS), $(OVMF_VARS_LOCAL))
QEMU_BOOT_DEPS := ovmf-fetch
endif
endif
else
$(error BOOT=uefi requires ARCH=x86_64)
endif
endif

QEMU_IMAGE_ARGS := -drive format=raw,file=bin/$(IMAGE_NAME).img
QEMU_USB_IMAGE_ARGS := \
	-drive if=none,id=usbstick,format=raw,file=bin/$(IMAGE_NAME).img \
	-device qemu-xhci,id=xhci \
	-device usb-storage,bus=xhci.0,drive=usbstick

ifeq ($(ARCH), riscv_64)
ifeq ($(BOOT), opensbi)
QEMU_BOOT_ARGS := -kernel bin/kernel_riscv64/boot/kernel64.elf
else ifeq ($(BOOT), bios)
QEMU_BOOT_ARGS := -bios bin/boot/$(ARCH)/riscv_boot.elf
else
$(error Unsupported BOOT='$(BOOT)' for $(ARCH))
endif
QEMU_ARGS += -global virtio-mmio.force-legacy=off
QEMU_IMAGE_ARGS := \
	-drive if=none,id=vdisk,format=raw,file=bin/$(IMAGE_NAME).img \
	-device virtio-blk-device,drive=vdisk
QEMU_USB_IMAGE_ARGS :=
endif
ifeq ($(ARCH), riscv_32)
ifeq ($(BOOT), opensbi)
QEMU_BOOT_ARGS := -kernel bin/kernel_riscv32/boot/kernel32.elf
else ifeq ($(BOOT), bios)
QEMU_BOOT_ARGS := -bios bin/boot/$(ARCH)/riscv_boot.elf
else
$(error Unsupported BOOT='$(BOOT)' for $(ARCH))
endif
QEMU_ARGS += -global virtio-mmio.force-legacy=off
QEMU_IMAGE_ARGS := \
	-drive if=none,id=vdisk,format=raw,file=bin/$(IMAGE_NAME).img \
	-device virtio-blk-device,drive=vdisk
QEMU_USB_IMAGE_ARGS :=
endif

.PHONY: ovmf-fetch ovmf-clean run run-usb run-usb-bios run-usb-uefi

ovmf-fetch:
	@python3 $(OVMF_FETCH_SCRIPT) "$(OVMF_DIR)" "$(OVMF_DEB_URL)"

ovmf-clean:
	@rm -rf "$(OVMF_DIR)"

run: all $(QEMU_BOOT_DEPS)
	$(QEMU_BOOT_SETUP)
	@if [ -r /dev/tty ]; then exec </dev/tty >/dev/tty 2>/dev/tty; fi; $(QEMU) $(QEMU_ARGS) $(QEMU_BOOT_ARGS) $(QEMU_IMAGE_ARGS)

run-usb: all $(QEMU_BOOT_DEPS)
	$(QEMU_BOOT_SETUP)
	@if [ -r /dev/tty ]; then exec </dev/tty >/dev/tty 2>/dev/tty; fi; $(QEMU) $(QEMU_ARGS) $(QEMU_BOOT_ARGS) $(QEMU_USB_IMAGE_ARGS)

run-usb-bios:
	@$(MAKE) run-usb BOOT=bios

run-usb-uefi:
	@$(MAKE) run-usb BOOT=uefi
