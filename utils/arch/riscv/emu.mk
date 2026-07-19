QEMU_ARCH    := riscv$(ARCH_VARIANT)
QEMU_MACHINE := virt

QEMU_ISO_SUPPORTED := false
QEMU_IMAGE_LOADER := \
	-bios none \
	-device loader,file=bin/$(IMAGE_NAME).img,addr=0x80000000,cpu-num=0,force-raw=on

SPIKE                  ?= spike
SPIKE_ISA              ?= rv$(ARCH_VARIANT)ima_zicsr_zifencei
SPIKE_RAM_MB           ?= 256
SPIKE_INITRD           ?= bin/$(IMAGE_NAME).rootfs.img
SPIKE_MMIO_UART        ?= true
SPIKE_MMIO_UART_PLUGIN ?= bin/spike_ns16550a_mmio.so
SPIKE_MMIO_UART_DEVICE ?= ns16550a
SPIKE_MMIO_UART_BASE   ?= 0x10000000
SPIKE_MMIO_UART_ARGS   ?= tty
SPIKE_MMIO_UART_CXX    ?= c++
SPIKE_REAL_TIME_CLINT  ?= false

SPIKE_MMIO_UART_ENABLED := $(filter true,$(SPIKE_MMIO_UART))
SPIKE_RT_CLINT_ON       := $(filter true,$(SPIKE_REAL_TIME_CLINT))
SPIKE_MMIO_UART_DEPS    :=
SPIKE_MMIO_UART_FLAGS   :=
SPIKE_CLINT_FLAGS       :=

ifeq ($(SPIKE_MMIO_UART_ENABLED),true)
SPIKE_MMIO_UART_DEPS += $(SPIKE_MMIO_UART_PLUGIN)
SPIKE_MMIO_UART_FLAGS += --extlib="$(SPIKE_MMIO_UART_PLUGIN)"
SPIKE_MMIO_UART_FLAGS += --device=$(SPIKE_MMIO_UART_DEVICE),$(SPIKE_MMIO_UART_BASE),$(SPIKE_MMIO_UART_ARGS)
endif

ifeq ($(SPIKE_RT_CLINT_ON),true)
SPIKE_CLINT_FLAGS += --real-time-clint
endif
