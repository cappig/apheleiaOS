USER_BIN_DIR := bin/user/$(ARCH_VARIANT)/bin
USER_OBJ_DIR := bin/user/$(ARCH_VARIANT)
USER_ROOT_DIR := root/sbin

USER_LIBC_SRC := \
	$(wildcard libs/libc/*.c) \
	$(wildcard libs/libc_ext/*.c) \
	$(wildcard libs/libc_usr/*.c)

USER_UTILS := ls cat echo pwd clear uname sleep head kill true false whoami id groups login ps
USER_PROGS := init sh $(USER_UTILS)

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
USER_CC += -DARCH_NAME=\"x86_64\"
USER_AS := -felf64
USER_LD := --gc-sections -Tuser/linker64.ld -melf_x86_64
USER_CRT_SRC := libs/libc_usr/arch/x86_64/crt0.asm
else ifeq ($(ARCH_VARIANT), 32)
USER_CC += -m32
USER_CC += -DARCH_NAME=\"x86_32\"
USER_AS := -felf32
USER_LD := --gc-sections -Tuser/linker32.ld -melf_i386
USER_CRT_SRC := libs/libc_usr/arch/x86_32/crt0.asm
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)')
endif

USER_LIBC_OBJ := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_LIBC_SRC))
USER_CRT_OBJ := $(patsubst %.asm, $(USER_OBJ_DIR)/%.asm.o, $(USER_CRT_SRC))

USER_PROGS_ROOT := $(foreach prog, $(USER_PROGS), $(USER_ROOT_DIR)/$(prog))

USER_BINARIES := $(USER_PROGS_ROOT)

$(USER_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(USER_CC), $@, $<)

$(USER_OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(USER_AS), $@, $<)

$(USER_BIN_DIR)/%: $(USER_CRT_OBJ) $(USER_LIBC_OBJ) $(USER_OBJ_DIR)/user/%/main.c.o $(USER_LIBGCC)
	@mkdir -p $(@D)
	$(call ld, $(USER_LD), $@, $^)

$(USER_ROOT_DIR)/%: $(USER_BIN_DIR)/%
	@mkdir -p $(@D)
	@cp $< $@

bin/$(IMG_NAME): $(USER_BINARIES)

.SECONDARY:
