# Emulator helpers shared by QEMU and Spike run targets.

# ---------------------------
# Architecture normalization
# ---------------------------
QEMU_ARCH := $(ARCH)

ifeq ($(ARCH), x86_32)
QEMU_ARCH := i386
else ifeq ($(ARCH), riscv_64)
QEMU_ARCH := riscv64
else ifeq ($(ARCH), riscv_32)
QEMU_ARCH := riscv32
endif

QEMU := qemu-system-$(QEMU_ARCH)

# ---------------------------
# Spike settings
# ---------------------------
SPIKE                  ?= spike
SPIKE_ISA              ?= rv$(ARCH_VARIANT)ia_zicsr
SPIKE_RAM_MB           ?= 256
SPIKE_INITRD           ?= bin/$(IMAGE_NAME).rootfs.img
SPIKE_MMIO_UART        ?= true
SPIKE_MMIO_UART_PLUGIN ?= bin/spike_ns16550a_mmio.so
SPIKE_MMIO_UART_DEVICE ?= ns16550a
SPIKE_MMIO_UART_BASE   ?= 0x10000000
SPIKE_MMIO_UART_ARGS   ?= tty
SPIKE_MMIO_UART_CXX    ?= c++
SPIKE_DISABLE_DTB      ?= false

SPIKE_MMIO_UART_ENABLED := $(filter true,$(SPIKE_MMIO_UART))
SPIKE_DISABLE_DTB_ENABLED := $(filter true,$(SPIKE_DISABLE_DTB))
SPIKE_MMIO_UART_DEPS :=
SPIKE_MMIO_UART_FLAGS :=
SPIKE_DTB_FLAGS :=
ifeq ($(SPIKE_MMIO_UART_ENABLED),true)
SPIKE_MMIO_UART_DEPS += $(SPIKE_MMIO_UART_PLUGIN)
SPIKE_MMIO_UART_FLAGS += --extlib="$(SPIKE_MMIO_UART_PLUGIN)"
SPIKE_MMIO_UART_FLAGS += --device=$(SPIKE_MMIO_UART_DEVICE),$(SPIKE_MMIO_UART_BASE),$(SPIKE_MMIO_UART_ARGS)
endif
ifeq ($(SPIKE_DISABLE_DTB_ENABLED),true)
SPIKE_DTB_FLAGS += --disable-dtb
endif

# ---------------------------
# QEMU settings
# ---------------------------
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
QEMU_BOOT_ARGS := \
	-bios none \
	-device loader,file=bin/$(IMAGE_NAME).img,addr=0x80000000,cpu-num=0,force-raw=on
QEMU_IMAGE_ARGS :=
QEMU_USB_IMAGE_ARGS :=
endif
ifeq ($(ARCH), riscv_32)
QEMU_BOOT_ARGS := \
	-bios none \
	-device loader,file=bin/$(IMAGE_NAME).img,addr=0x80000000,cpu-num=0,force-raw=on
QEMU_IMAGE_ARGS :=
QEMU_USB_IMAGE_ARGS :=
endif

.PHONY: ovmf-fetch ovmf-clean run run-usb run-usb-bios run-usb-uefi run-spike run-spike-no-fdt

$(SPIKE_MMIO_UART_PLUGIN): utils/spike/mmio_uart_plugin.cpp
	@mkdir -p "$(dir $@)"
	@if ! command -v "$(SPIKE_MMIO_UART_CXX)" >/dev/null 2>&1; then \
		echo "Missing host C++ compiler '$(SPIKE_MMIO_UART_CXX)' for Spike MMIO UART plugin build."; \
		echo "Install one (e.g. g++ or clang++) or override with SPIKE_MMIO_UART_CXX=<compiler>."; \
		exit 1; \
	fi
	@echo "Building Spike MMIO UART plugin: $@"
	@"$(SPIKE_MMIO_UART_CXX)" -std=c++17 -O2 -fPIC -shared -Wall -Wextra -Wpedantic \
		-I/usr/include \
		-o "$@" "$<"

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

run-spike: all $(SPIKE_MMIO_UART_DEPS)
	@if [ "$(ARCH_TREE)" != "riscv" ]; then \
		echo "run-spike supports only ARCH=riscv_32 or ARCH=riscv_64"; \
		exit 1; \
	fi
	@if ! command -v "$(SPIKE)" >/dev/null 2>&1; then \
		echo "spike was not found in PATH (override with SPIKE=/path/to/spike)"; \
		exit 1; \
	fi
	@if [ ! -f "$(SPIKE_INITRD)" ]; then \
		echo "Spike initrd image not found: $(SPIKE_INITRD)"; \
		echo "Build it with: make ARCH=$(ARCH) TOOLCHAIN=$(TOOLCHAIN)"; \
		exit 1; \
	fi
	@if [ "$(SPIKE_MMIO_UART_ENABLED)" = "true" ] && [ ! -f "$(SPIKE_MMIO_UART_PLUGIN)" ]; then \
		echo "Spike MMIO UART plugin not found: $(SPIKE_MMIO_UART_PLUGIN)"; \
		exit 1; \
	fi
	@echo "Launching Spike with $(BOOT_ENTRY_ELF)"
	@echo "Using initrd rootfs: $(SPIKE_INITRD)"
	@if [ "$(SPIKE_MMIO_UART_ENABLED)" = "true" ]; then \
		echo "Spike MMIO UART: plugin=$(SPIKE_MMIO_UART_PLUGIN) device=$(SPIKE_MMIO_UART_DEVICE) base=$(SPIKE_MMIO_UART_BASE) args=$(SPIKE_MMIO_UART_ARGS)"; \
	else \
		echo "Spike MMIO UART disabled (SPIKE_MMIO_UART=false); kernel will run without UART console if UART base is 0."; \
	fi
	@if [ "$(SPIKE_DISABLE_DTB_ENABLED)" = "true" ]; then \
		echo "Spike DTB disabled (--disable-dtb): boot path will run with no FDT."; \
	fi
	@if [ "$(SPIKE_MMIO_UART_ARGS)" = "tty" ]; then \
		$(SPIKE) --isa=$(SPIKE_ISA) -m$(SPIKE_RAM_MB) $(SPIKE_MMIO_UART_FLAGS) $(SPIKE_DTB_FLAGS) --initrd="$(SPIKE_INITRD)" "$(BOOT_ENTRY_ELF)" </dev/null; \
	else \
		$(SPIKE) --isa=$(SPIKE_ISA) -m$(SPIKE_RAM_MB) $(SPIKE_MMIO_UART_FLAGS) $(SPIKE_DTB_FLAGS) --initrd="$(SPIKE_INITRD)" "$(BOOT_ENTRY_ELF)"; \
	fi

run-spike-no-fdt:
	@$(MAKE) run-spike SPIKE_DISABLE_DTB=true
