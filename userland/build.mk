USERLAND_TOOLS  ?= all
USERLAND_UI     ?= all
USERLAND_EXTRAS ?= all
USERLAND_GAMES  ?= all

USER_OBJ_DIR                := bin/user/$(ARCH)
USER_BIN_DIR                := $(USER_OBJ_DIR)/bin
USER_STAGE_ROOT_DIR         := $(USER_OBJ_DIR)/root
USER_STAGE_DIR              := $(USER_STAGE_ROOT_DIR)/bin
USER_STAGE_HOME_USER_DIR    := $(USER_STAGE_ROOT_DIR)/home/user
USER_STAGE_USR_DIR          := $(USER_STAGE_ROOT_DIR)/usr
USER_STAGE_USR_INCLUDE_DIR  := $(USER_STAGE_USR_DIR)/include
USER_STAGE_USR_LIB_DIR      := $(USER_STAGE_USR_DIR)/lib

USER_LIBC_SRC := \
	$(wildcard libs/libc/*.c) \
	$(wildcard libs/libc_ext/*.c) \
	$(wildcard libs/libc_usr/*.c)

ifeq ($(ARCH_TREE), riscv)
USER_LIBC_SRC := $(filter-out \
	libs/libc/math.c \
	libs/libc_usr/strtod.c, \
	$(USER_LIBC_SRC))
endif
USER_COMMON_SRC := $(wildcard libs/user/*.c)
USER_DATA_SRC   := $(wildcard libs/data/*.c)
USER_GUI_SRC    := $(wildcard libs/gui/*.c)
USER_TERM_SRC   := $(wildcard libs/term/*.c)
USER_PARSE_SRC  := libs/parse/psf.c libs/parse/ppm.c libs/parse/textdb.c

ifeq ($(ARCH_TREE), riscv)
USER_CRT_SRC  := libs/libc_usr/arch/riscv_$(ARCH_VARIANT)/crt0.S
USER_CRTI_SRC := libs/libc_usr/arch/riscv_$(ARCH_VARIANT)/crti.S
USER_CRTN_SRC := libs/libc_usr/arch/riscv_$(ARCH_VARIANT)/crtn.S
USER_LD_SCRIPT := userland/linker_riscv$(ARCH_VARIANT).ld

USER_RISCV_64_ISA_FLAGS := -march=rv64ia_zicsr -mabi=lp64
USER_RISCV_32_ISA_FLAGS := -march=rv32ia_zicsr -mabi=ilp32

ifeq ($(ARCH_VARIANT), 64)
USER_ARCH_NAME := riscv_64
USER_LD_EMU    := -melf64lriscv
USER_ARCH_CFLAGS := $(USER_RISCV_64_ISA_FLAGS) -mcmodel=medlow
else ifeq ($(ARCH_VARIANT), 32)
USER_ARCH_NAME := riscv_32
USER_LD_EMU    := -melf32lriscv
USER_ARCH_CFLAGS := $(USER_RISCV_32_ISA_FLAGS) -mcmodel=medlow
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)')
endif
else
USER_CRT_SRC  := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crt0.asm
USER_CRTI_SRC := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crti.asm
USER_CRTN_SRC := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crtn.asm
USER_LD_SCRIPT := userland/linker$(ARCH_VARIANT).ld

ifeq ($(ARCH_VARIANT), 64)
USER_ARCH_NAME := x86_64
USER_LD_EMU    := -melf_x86_64
USER_ARCH_CFLAGS := -m64
else ifeq ($(ARCH_VARIANT), 32)
USER_ARCH_NAME := x86_32
USER_LD_EMU    := -melf_i386
USER_ARCH_CFLAGS := -m32
else
$(error Unsupported ARCH_VARIANT '$(ARCH_VARIANT)')
endif
endif

USER_CC := \
	-nostdlib \
	-ffunction-sections \
	-fdata-sections \
	-Ilibs/libc_usr \
	-Ilibs/user \
	-Ilibs/gui \
	$(USER_ARCH_CFLAGS) \
	-DARCH_NAME=\"$(USER_ARCH_NAME)\"

USER_AS := -felf$(ARCH_VARIANT)

USER_LD := \
	--gc-sections \
	-T$(USER_LD_SCRIPT) \
	$(USER_LD_EMU)

USER_LIBGCC := $(call LIBGCC, $(USER_CC))

USER_CORE_PROG_DIRS  := $(sort $(patsubst %/main.c, %, $(wildcard userland/core/*/main.c)))
USER_UI_ALL_DIRS     := $(sort $(patsubst %/main.c, %, $(wildcard userland/ui/*/main.c)))
USER_TOOLS_ALL_DIRS  := $(sort $(patsubst %/main.c, %, $(wildcard userland/tools/*/main.c)))
USER_EXTRA_ALL_DIRS  := $(sort $(patsubst %/main.c, %, $(wildcard userland/extra/*/main.c)))
USER_GAMES_ALL_DIRS  := $(sort $(patsubst %/main.c, %, $(wildcard userland/games/*/main.c)))

define user_makefile_dirs
$(notdir $(patsubst %/,%,$(dir $(wildcard $(1)/*/Makefile))))
endef

USER_TOOLS_OPTION_NAMES := $(sort \
	$(notdir $(USER_TOOLS_ALL_DIRS)) \
	$(call user_makefile_dirs,userland/tools) \
)
USER_UI_OPTION_NAMES := $(sort $(notdir $(USER_UI_ALL_DIRS)))
USER_EXTRA_OPTION_NAMES := $(sort \
	$(notdir $(USER_EXTRA_ALL_DIRS)) \
	$(call user_makefile_dirs,userland/extra) \
)
USER_GAMES_OPTION_NAMES := $(sort \
	$(notdir $(USER_GAMES_ALL_DIRS)) \
	$(call user_makefile_dirs,userland/games) \
)

USER_TOOLS_SELECTED_NAMES := $(USERLAND_TOOLS)
ifeq ($(USERLAND_TOOLS), all)
USER_TOOLS_SELECTED_NAMES := $(USER_TOOLS_OPTION_NAMES)
else ifeq ($(USERLAND_TOOLS), none)
USER_TOOLS_SELECTED_NAMES :=
endif

USER_UI_SELECTED_NAMES := $(USERLAND_UI)
ifeq ($(USERLAND_UI), all)
USER_UI_SELECTED_NAMES := $(USER_UI_OPTION_NAMES)
else ifeq ($(USERLAND_UI), none)
USER_UI_SELECTED_NAMES :=
endif

USER_EXTRA_SELECTED_NAMES := $(USERLAND_EXTRAS)
ifeq ($(USERLAND_EXTRAS), all)
USER_EXTRA_SELECTED_NAMES := $(USER_EXTRA_OPTION_NAMES)
else ifeq ($(USERLAND_EXTRAS), none)
USER_EXTRA_SELECTED_NAMES :=
endif

USER_GAMES_SELECTED_NAMES := $(USERLAND_GAMES)
ifeq ($(USERLAND_GAMES), all)
USER_GAMES_SELECTED_NAMES := $(USER_GAMES_OPTION_NAMES)
else ifeq ($(USERLAND_GAMES), none)
USER_GAMES_SELECTED_NAMES :=
endif

USER_TOOLS_UNKNOWN := $(filter-out $(USER_TOOLS_OPTION_NAMES),$(USER_TOOLS_SELECTED_NAMES))
ifneq ($(strip $(USER_TOOLS_UNKNOWN)),)
$(error Unknown tools selection(s) in USERLAND_TOOLS: $(USER_TOOLS_UNKNOWN))
endif

USER_UI_UNKNOWN := $(filter-out $(USER_UI_OPTION_NAMES),$(USER_UI_SELECTED_NAMES))
ifneq ($(strip $(USER_UI_UNKNOWN)),)
$(error Unknown UI selection(s) in USERLAND_UI: $(USER_UI_UNKNOWN))
endif

USER_EXTRA_UNKNOWN := $(filter-out $(USER_EXTRA_OPTION_NAMES),$(USER_EXTRA_SELECTED_NAMES))
ifneq ($(strip $(USER_EXTRA_UNKNOWN)),)
$(error Unknown extra selection(s) in USERLAND_EXTRAS: $(USER_EXTRA_UNKNOWN))
endif

USER_GAMES_UNKNOWN := $(filter-out $(USER_GAMES_OPTION_NAMES),$(USER_GAMES_SELECTED_NAMES))
ifneq ($(strip $(USER_GAMES_UNKNOWN)),)
$(error Unknown game selection(s) in USERLAND_GAMES: $(USER_GAMES_UNKNOWN))
endif

USER_TOOLS_PROG_DIRS := $(filter $(addprefix userland/tools/,$(USER_TOOLS_SELECTED_NAMES)),$(USER_TOOLS_ALL_DIRS))
USER_UI_PROG_DIRS    := $(filter $(addprefix userland/ui/,$(USER_UI_SELECTED_NAMES)),$(USER_UI_ALL_DIRS))
USER_EXTRA_PROG_DIRS := $(filter $(addprefix userland/extra/,$(USER_EXTRA_SELECTED_NAMES)),$(USER_EXTRA_ALL_DIRS))
USER_GAMES_PROG_DIRS := $(filter $(addprefix userland/games/,$(USER_GAMES_SELECTED_NAMES)),$(USER_GAMES_ALL_DIRS))

USER_PROG_DIRS := $(sort \
	$(USER_CORE_PROG_DIRS) \
	$(USER_UI_PROG_DIRS) \
	$(USER_TOOLS_PROG_DIRS) \
	$(USER_EXTRA_PROG_DIRS) \
	$(USER_GAMES_PROG_DIRS) \
)
USER_APP_SRC := $(foreach prog_dir,$(USER_PROG_DIRS),$(wildcard $(prog_dir)/*.c))
USER_PROGS   := $(notdir $(USER_PROG_DIRS))

ifneq ($(words $(USER_PROGS)), $(words $(sort $(USER_PROGS))))
$(error Duplicate userspace program names found across userland subdirectories)
endif

USER_CRT_OBJ  := $(patsubst %, $(USER_OBJ_DIR)/%.o, $(USER_CRT_SRC))
USER_CRTI_OBJ := $(patsubst %, $(USER_OBJ_DIR)/%.o, $(USER_CRTI_SRC))
USER_CRTN_OBJ := $(patsubst %, $(USER_OBJ_DIR)/%.o, $(USER_CRTN_SRC))

USER_LIBC_OBJ   := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_LIBC_SRC))
USER_COMMON_OBJ := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_COMMON_SRC))
USER_DATA_OBJ   := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_DATA_SRC))
USER_GUI_OBJ    := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_GUI_SRC))
USER_TERM_OBJ   := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_TERM_SRC))
USER_PARSE_OBJ  := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_PARSE_SRC))
USER_APP_OBJ    := $(patsubst %.c, $(USER_OBJ_DIR)/%.c.o, $(USER_APP_SRC))

USER_USR_INCLUDE_HEADERS := \
	$(wildcard libs/libc/*.h) \
	$(wildcard libs/libc/sys/*.h) \
	$(wildcard libs/libc_usr/*.h) \
	$(wildcard libs/libc_ext/*.h)

USER_USR_OBJ_DIR     := $(USER_OBJ_DIR)/usr
USER_USR_LIBC_A      := $(USER_USR_OBJ_DIR)/libc.a
USER_USR_CRT1_OBJ    := $(USER_USR_OBJ_DIR)/crt1.o
USER_USR_CRTI_OBJ    := $(USER_USR_OBJ_DIR)/crti.o
USER_USR_CRTN_OBJ    := $(USER_USR_OBJ_DIR)/crtn.o
USER_USR_RUNTIME_OBJ := $(USER_USR_LIBC_A) $(USER_USR_CRT1_OBJ) \
	$(USER_USR_CRTI_OBJ) $(USER_USR_CRTN_OBJ)

USER_STAGE_USR_INCLUDE_STAMP := $(USER_STAGE_USR_INCLUDE_DIR)/.apheleia_headers
USER_STAGE_USR_LIB_STAMP     := $(USER_STAGE_USR_LIB_DIR)/.apheleia_runtime

$(USER_USR_LIBC_A): $(USER_LIBC_OBJ)
	@mkdir -p $(@D)
	@rm -f $@
	@ar rcs $@ $^

$(USER_USR_CRT1_OBJ): $(USER_CRT_OBJ)
	@mkdir -p $(@D)
	@cp $< $@

$(USER_USR_CRTI_OBJ): $(USER_CRTI_OBJ)
	@mkdir -p $(@D)
	@cp $< $@

$(USER_USR_CRTN_OBJ): $(USER_CRTN_OBJ)
	@mkdir -p $(@D)
	@cp $< $@

$(USER_STAGE_USR_INCLUDE_STAMP): $(USER_USR_INCLUDE_HEADERS)
	@mkdir -p "$(USER_STAGE_USR_INCLUDE_DIR)/sys" \
		"$(USER_STAGE_USR_INCLUDE_DIR)/libc_usr" \
		"$(USER_STAGE_USR_INCLUDE_DIR)/libc_ext"
	@cp -f libs/libc/*.h "$(USER_STAGE_USR_INCLUDE_DIR)/"
	@cp -f libs/libc/sys/*.h "$(USER_STAGE_USR_INCLUDE_DIR)/sys/"
	@cp -f libs/libc_usr/*.h "$(USER_STAGE_USR_INCLUDE_DIR)/libc_usr/"
	@cp -f libs/libc_ext/*.h "$(USER_STAGE_USR_INCLUDE_DIR)/libc_ext/"
	@touch $@

$(USER_STAGE_USR_LIB_STAMP): $(USER_USR_RUNTIME_OBJ)
	@mkdir -p "$(USER_STAGE_USR_LIB_DIR)"
	@cp "$(USER_USR_LIBC_A)" "$(USER_STAGE_USR_LIB_DIR)/libc.a"
	@cp "$(USER_USR_CRT1_OBJ)" "$(USER_STAGE_USR_LIB_DIR)/crt1.o"
	@cp "$(USER_USR_CRTI_OBJ)" "$(USER_STAGE_USR_LIB_DIR)/crti.o"
	@cp "$(USER_USR_CRTN_OBJ)" "$(USER_STAGE_USR_LIB_DIR)/crtn.o"
	@touch $@

USER_SHARED_OBJ := $(USER_CRT_OBJ) $(USER_LIBC_OBJ) $(USER_COMMON_OBJ) \
	$(USER_DATA_OBJ) $(USER_GUI_OBJ) $(USER_TERM_OBJ) $(USER_PARSE_OBJ)

USER_PROGS_BIN := $(addprefix $(USER_BIN_DIR)/,$(USER_PROGS))
USER_BINARIES  := $(addprefix $(USER_STAGE_DIR)/,$(USER_PROGS))
USER_BINARIES  += $(USER_STAGE_USR_INCLUDE_STAMP) $(USER_STAGE_USR_LIB_STAMP)

USER_SELECTED_TOOL_DIRS  := $(addprefix userland/tools/,$(USER_TOOLS_SELECTED_NAMES))
USER_SELECTED_EXTRA_DIRS := $(addprefix userland/extra/,$(USER_EXTRA_SELECTED_NAMES))
USER_SELECTED_GAME_DIRS  := $(addprefix userland/games/,$(USER_GAMES_SELECTED_NAMES))

USERLAND_INTEGRATION := true
USER_PROG_MAKEFILES := $(sort $(wildcard \
	$(USER_CORE_PROG_DIRS:%=%/Makefile) \
	$(USER_UI_PROG_DIRS:%=%/Makefile) \
	$(USER_SELECTED_TOOL_DIRS:%=%/Makefile) \
	$(USER_SELECTED_EXTRA_DIRS:%=%/Makefile) \
	$(USER_SELECTED_GAME_DIRS:%=%/Makefile) \
))
-include $(USER_PROG_MAKEFILES)

STRIP_USER       ?= true
USER_STRIP_FLAGS ?= --strip-debug

.SECONDARY: $(USER_SHARED_OBJ) $(USER_CRTI_OBJ) $(USER_CRTN_OBJ) \
	$(USER_APP_OBJ) $(USER_PROGS_BIN)

define user_prog_objs
$(foreach obj,$(USER_APP_OBJ),$(if $(findstring /$(1)/,$(obj)),$(obj)))
endef

$(USER_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(USER_CC), $@, $<)

$(USER_OBJ_DIR)/%.S.o: %.S
	@mkdir -p $(@D)
	$(call cc, $(USER_CC), $@, $<)

$(USER_OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(USER_AS), $@, $<)

define user_link_rule
$(USER_BIN_DIR)/$(1): $(USER_SHARED_OBJ) $(USER_LIBGCC) $(call user_prog_objs,$(1)) $(USER_LD_SCRIPT)
	@mkdir -p $$(@D)
	$(call ld, $(USER_LD), $$@, $$(filter-out $(USER_LD_SCRIPT),$$^))
	@if [ "$(STRIP_USER)" = "true" ]; then \
		$(ST) $(USER_STRIP_FLAGS) $$@; \
	fi
endef

$(foreach prog,$(USER_PROGS),$(eval $(call user_link_rule,$(prog))))

$(USER_STAGE_DIR)/%: $(USER_BIN_DIR)/%
	@mkdir -p $(@D)
	@cp $< $@

bin/$(IMAGE_NAME).img: $(USER_BINARIES)
bin/$(IMAGE_NAME).iso: $(USER_BINARIES)
