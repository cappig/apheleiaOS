EMU := qemu-system-x86_64

KVM ?= true
DEBUG ?= false

EMU_ARGS := \
	-no-reboot \
	-m 128M

ifeq ($(DEBUG), true)
	EMU_ARGS += -s -monitor stdio -d int,cpu_reset,guest_errors -M smm=off
else
	EMU_ARGS += -serial stdio
endif

ifeq ($(KVM), true)
	EMU_ARGS += -enable-kvm -cpu host
endif

.PHONY: run
run:
	$(EMU) $(EMU_ARGS) -drive format=raw,file=bin/$(IMG_NAME)
