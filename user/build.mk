USER_OBJ_DIR   := bin/user/$(ARCH_VARIANT)
USER_BIN_DIR   := $(USER_OBJ_DIR)/bin
USER_STAGE_DIR := $(USER_OBJ_DIR)/root/bin

USER_LIBC_SRC   := $(wildcard libs/libc/*.c) $(wildcard libs/libc_ext/*.c) $(wildcard libs/libc_usr/*.c)
USER_COMMON_SRC := $(wildcard libs/user/*.c)
USER_DATA_SRC   := $(wildcard libs/data/*.c)
USER_GUI_SRC    := $(wildcard libs/gui/*.c)
USER_TERM_SRC   := $(wildcard libs/term/*.c)
USER_PARSE_SRC  := libs/parse/psf.c libs/parse/ppm.c libs/parse/textdb.c
USER_APP_SRC  := $(wildcard user/*/*.c)
USER_MAIN_SRC := $(wildcard user/*/main.c)
USER_PROGS    := $(patsubst user/%/main.c, %, $(USER_MAIN_SRC))

USER_CRT_SRC := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crt0.asm


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
	-nostdlib \
	-Ilibs/libc_usr \
	-Ilibs/user \
	-Ilibs/gui \
	-m$(ARCH_VARIANT) \
	-DARCH_NAME=\"$(USER_ARCH_NAME)\"

USER_AS := -felf$(ARCH_VARIANT)

USER_LD := \
	--gc-sections \
	-Tuser/linker$(ARCH_VARIANT).ld \
	$(USER_LD_EMU)


USER_LIBGCC := $(call LIBGCC, $(USER_CC))

USER_LIBC_OBJ   := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_LIBC_SRC))
USER_CRT_OBJ    := $(patsubst %.asm, $(USER_OBJ_DIR)/%.asm.o, $(USER_CRT_SRC))
USER_APP_OBJ    := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_APP_SRC))
USER_COMMON_OBJ := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_COMMON_SRC))
USER_DATA_OBJ   := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_DATA_SRC))
USER_GUI_OBJ    := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_GUI_SRC))
USER_TERM_OBJ   := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_TERM_SRC))
USER_PARSE_OBJ  := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_PARSE_SRC))

USER_PROGS_BIN := $(foreach prog, $(USER_PROGS), $(USER_BIN_DIR)/$(prog))
USER_BINARIES  := $(foreach prog, $(USER_PROGS), $(USER_STAGE_DIR)/$(prog))

.SECONDARY: $(USER_LIBC_OBJ) $(USER_CRT_OBJ) $(USER_APP_OBJ) $(USER_COMMON_OBJ) \
            $(USER_DATA_OBJ) $(USER_GUI_OBJ) $(USER_TERM_OBJ) $(USER_PARSE_OBJ) $(USER_PROGS_BIN)


$(USER_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(USER_CC), $@, $<)

$(USER_OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(USER_AS), $@, $<)

define USER_LINK_RULE
$(USER_BIN_DIR)/$(1): $(USER_CRT_OBJ) $(USER_LIBC_OBJ) $(USER_COMMON_OBJ) $(USER_DATA_OBJ) $(USER_GUI_OBJ) $(USER_TERM_OBJ) $(USER_PARSE_OBJ) $$(filter $(USER_OBJ_DIR)/user/$(1)/%.c.o,$(USER_APP_OBJ)) $(USER_LIBGCC)
	@mkdir -p $$(@D)
	$$(call ld, $(USER_LD), $$@, $$^)
endef

$(foreach prog, $(USER_PROGS), $(eval $(call USER_LINK_RULE,$(prog))))

$(USER_STAGE_DIR)/%: $(USER_BIN_DIR)/%
	@mkdir -p $(@D)
	@cp $< $@

bin/$(IMAGE_NAME).img: $(USER_BINARIES)
bin/$(IMAGE_NAME).iso: $(USER_BINARIES)
