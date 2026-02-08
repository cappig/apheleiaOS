ifeq ($(ARCH), x86_64)
	QEMU := qemu-system-x86_64
else ifeq ($(ARCH), x86_32)
	QEMU := qemu-system-i386
else ifeq ($(ARCH), riscv_64)
	QEMU := qemu-system-riscv64
else
	QEMU := qemu-system-$(ARCH)
endif

KVM ?= true
QEMU_CONSOLE ?= false
SERIAL_CONSOLE ?= stdio

QEMU_ARGS := \
	-no-reboot \
	-m 64M

# Console configuration
ifeq ($(QEMU_CONSOLE), true)
	QEMU_ARGS += -s -monitor stdio -d int,cpu_reset,guest_errors,mmu
else
	QEMU_ARGS += -serial stdio
endif

# ifeq ($(KVM), true)
# ifeq ($(ARCH), x86_64)
# 	EMU_ARGS += -enable-kvm -cpu host
# endif
# endif

.PHONY: run
run:
	$(QEMU) $(QEMU_ARGS) -drive format=raw,file=bin/$(IMG_NAME)
