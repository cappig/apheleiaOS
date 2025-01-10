EMU := qemu-system-x86_64

KVM ?= true
QEMU_CONSOLE ?= false

EMU_ARGS := \
	-no-reboot \
	-m 64M

ifeq ($(QEMU_CONSOLE), true)
	EMU_ARGS += -s -monitor stdio -d int,cpu_reset,guest_errors,mmu
else
	EMU_ARGS += -serial stdio
endif

ifeq ($(KVM), true)
	EMU_ARGS += -enable-kvm -cpu host
endif

.PHONY: run mbr
run:
	$(EMU) $(EMU_ARGS) -drive media=cdrom,file=bin/$(IMG_NAME)
mbr:
	$(EMU) $(EMU_ARGS) -drive format=raw,file=bin/$(IMG_NAME)
