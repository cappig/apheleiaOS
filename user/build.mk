USER_BIN_DIR := bin/user/$(ARCH_VARIANT)/bin
USER_OBJ_DIR := bin/user/$(ARCH_VARIANT)
USER_ROOT_DIR := root/sbin

USER_LIBC_SRC := \
	$(wildcard libs/libc/*.c) \
	$(wildcard libs/libc_ext/*.c) \
	$(wildcard libs/libc_usr/*.c)

USER_INIT_SRC := user/init/main.c
USER_SH_SRC := user/sh/main.c

USER_CRT_SRC :=

USER_CC := \
	$(CC_BASE) \
	-ffreestanding \
	-nostdlib \
	-nostdinc \
	-Ilibs \
	-Ilibs/libc \
	-Ilibs/libc_usr

USER_AS :=
USER_LD :=

ifeq ($(ARCH_VARIANT), 64)
USER_CC += -m64
USER_AS := -felf64
USER_LD := --gc-sections -Tuser/linker64.ld -melf_x86_64
USER_CRT_SRC := libs/libc_usr/arch/x86_64/crt0.asm
else ifeq ($(ARCH_VARIANT), 32)
USER_CC += -m32
USER_AS := -felf32
USER_LD := --gc-sections -Tuser/linker32.ld -melf_i386
USER_CRT_SRC := libs/libc_usr/arch/x86_32/crt0.asm
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)')
endif

USER_LIBC_OBJ := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_LIBC_SRC))
USER_INIT_OBJ := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_INIT_SRC))
USER_SH_OBJ := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_SH_SRC))
USER_CRT_OBJ := $(patsubst %.asm, $(USER_OBJ_DIR)/%.asm.o, $(USER_CRT_SRC))

USER_INIT_ELF := $(USER_BIN_DIR)/init.elf
USER_SH_ELF := $(USER_BIN_DIR)/sh.elf
USER_ROOT_INIT := $(USER_ROOT_DIR)/init.elf
USER_ROOT_SH := $(USER_ROOT_DIR)/sh.elf

USER_BINARIES := $(USER_ROOT_INIT) $(USER_ROOT_SH)

$(USER_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(USER_CC), $@, $<)

$(USER_OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(USER_AS), $@, $<)

$(USER_INIT_ELF): $(USER_CRT_OBJ) $(USER_LIBC_OBJ) $(USER_INIT_OBJ)
	@mkdir -p $(@D)
	$(call ld, $(USER_LD), $@, $^)

$(USER_SH_ELF): $(USER_CRT_OBJ) $(USER_LIBC_OBJ) $(USER_SH_OBJ)
	@mkdir -p $(@D)
	$(call ld, $(USER_LD), $@, $^)

$(USER_ROOT_INIT): $(USER_INIT_ELF) FORCE
	@mkdir -p $(@D)
	@cp $< $@

$(USER_ROOT_SH): $(USER_SH_ELF) FORCE
	@mkdir -p $(@D)
	@cp $< $@

bin/$(IMG_NAME): $(USER_BINARIES)

.PHONY: FORCE
FORCE:
