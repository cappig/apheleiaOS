DRIVER_ARCH_DIR := kernel/arch/$(ARCH_TREE)/drivers
DRIVER_REGISTRY_SRC := $(DRIVER_ARCH_DIR)/registry.c

ifeq ($(wildcard $(DRIVER_REGISTRY_SRC)),)
$(error Missing driver registry for ARCH_TREE '$(ARCH_TREE)' at $(DRIVER_REGISTRY_SRC))
endif

DRIVER_COMMON_SRC := \
	$(wildcard kernel/drivers/*.c)

DRIVER_COMMON_ASM := \
	$(wildcard kernel/drivers/*.asm)

DRIVER_ARCH_SRC := \
	$(wildcard $(DRIVER_ARCH_DIR)/*.c) \
	$(wildcard $(DRIVER_ARCH_DIR)/*.asm)

KERNEL_DRIVER_SRC := $(sort $(DRIVER_COMMON_SRC) $(DRIVER_COMMON_ASM) $(DRIVER_ARCH_SRC))
