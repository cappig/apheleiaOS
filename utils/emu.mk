# Emulator helpers shared by all targets.

include utils/arch/$(ARCH_TREE)/emu.mk

QEMU := qemu-system-$(QEMU_ARCH)

# ---------------------------
# qemu settings
# ---------------------------
QEMU_CONSOLE  ?= false
QEMU_MEMORY   ?= 256M
QEMU_CPU      ?= max
QEMU_SMP      ?= 1
QEMU_KVM      ?= false
QEMU_SNAPSHOT ?= false
QEMU_MACHINE  ?=

ifeq ($(QEMU_KVM), true)
ifeq ($(QEMU_CPU), max)
QEMU_CPU = host
endif
endif

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

ifeq ($(QEMU_KVM), true)
QEMU_ARGS += -enable-kvm
endif

ifneq ($(QEMU_MACHINE),)
QEMU_ARGS += -machine $(QEMU_MACHINE)
endif

ifeq ($(QEMU_SNAPSHOT), true)
QEMU_ARGS += -snapshot
endif

QEMU_BOOT_ARGS :=

ifeq ($(IMAGE_FORMAT),iso)
ifneq ($(QEMU_ISO_SUPPORTED),true)
$(error IMAGE_FORMAT=iso is not supported for ARCH=$(ARCH))
endif
QEMU_IMAGE_ARGS := -cdrom bin/$(IMAGE_NAME).iso -boot d
else ifneq ($(QEMU_IMAGE_LOADER),)
QEMU_BOOT_ARGS  := $(QEMU_IMAGE_LOADER)
QEMU_IMAGE_ARGS :=
else
QEMU_IMAGE_ARGS := -drive format=raw,file=bin/$(IMAGE_NAME).img
endif

.PHONY: run

run: all
	@if tty -s; then exec </dev/tty >/dev/tty 2>/dev/tty; fi; $(QEMU) $(QEMU_ARGS) $(QEMU_BOOT_ARGS) $(QEMU_IMAGE_ARGS)

include utils/arch/$(ARCH_TREE)/emu_targets.mk
