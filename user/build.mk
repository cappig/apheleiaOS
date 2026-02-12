USER_BIN_DIR := bin/user/$(ARCH_VARIANT)/bin
USER_OBJ_DIR := bin/user/$(ARCH_VARIANT)
USER_STAGE_DIR := bin/user/$(ARCH_VARIANT)/root/sbin

USER_LIBC_SRC := \
	$(wildcard libs/libc/*.c) \
	$(wildcard libs/libc_ext/*.c) \
	$(wildcard libs/libc_usr/*.c)

USER_MAIN_SRC := $(wildcard user/*/main.c)
USER_PROGS := $(patsubst user/%/main.c,%,$(USER_MAIN_SRC))

ifeq ($(ARCH_VARIANT), 64)
USER_ARCH_NAME := x86_64
USER_LD_EMU := -melf_x86_64
else ifeq ($(ARCH_VARIANT), 32)
USER_ARCH_NAME := x86_32
USER_LD_EMU := -melf_i386
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)')
endif

USER_CC := \
	$(CC_BASE) \
	-ffreestanding \
	-nostdlib \
	-nostdinc \
	-Ilibs \
	-Ilibs/libc \
	-Ilibs/libc_usr \
	-m$(ARCH_VARIANT) \
	-DARCH_NAME=\"$(USER_ARCH_NAME)\"
USER_AS := -felf$(ARCH_VARIANT)
USER_LD := \
	--gc-sections \
	-Tuser/linker$(ARCH_VARIANT).ld \
	$(USER_LD_EMU)
USER_CRT_SRC := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crt0.asm
USER_LIBGCC := $(call LIBGCC, $(USER_CC))

USER_LIBC_OBJ := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_LIBC_SRC))
USER_CRT_OBJ := $(patsubst %.asm, $(USER_OBJ_DIR)/%.asm.o, $(USER_CRT_SRC))
USER_MAIN_OBJ := $(patsubst user/%/main.c,$(USER_OBJ_DIR)/user/%/main.c.o,$(USER_MAIN_SRC))

USER_PROGS_BIN := $(foreach prog, $(USER_PROGS), $(USER_BIN_DIR)/$(prog))

USER_PROGS_ROOT := $(foreach prog, $(USER_PROGS), $(USER_STAGE_DIR)/$(prog))

USER_BINARIES := $(USER_PROGS_ROOT)

.SECONDARY: $(USER_LIBC_OBJ) $(USER_CRT_OBJ) $(USER_MAIN_OBJ) $(USER_PROGS_BIN)

$(USER_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(USER_CC), $@, $<)

$(USER_OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(USER_AS), $@, $<)

$(USER_BIN_DIR)/%: $(USER_CRT_OBJ) $(USER_LIBC_OBJ) $(USER_OBJ_DIR)/user/%/main.c.o $(USER_LIBGCC)
	@mkdir -p $(@D)
	$(call ld, $(USER_LD), $@, $^)

$(USER_STAGE_DIR)/%: $(USER_BIN_DIR)/%
	@mkdir -p $(@D)
	@cp $< $@

bin/$(IMG_NAME): $(USER_BINARIES)
