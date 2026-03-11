USER_OBJ_DIR   := bin/user/$(ARCH_VARIANT)
USER_BIN_DIR   := $(USER_OBJ_DIR)/bin
USER_STAGE_ROOT_DIR := $(USER_OBJ_DIR)/root
USER_STAGE_DIR := $(USER_STAGE_ROOT_DIR)/bin
USER_STAGE_HOME_USER_DIR := $(USER_STAGE_ROOT_DIR)/home/user
USER_STAGE_USR_DIR := $(USER_STAGE_ROOT_DIR)/usr
USER_STAGE_USR_INCLUDE_DIR := $(USER_STAGE_USR_DIR)/include
USER_STAGE_USR_LIB_DIR := $(USER_STAGE_USR_DIR)/lib

USER_LIBC_SRC   := $(wildcard libs/libc/*.c) $(wildcard libs/libc_ext/*.c) \
                   $(wildcard libs/libc_usr/*.c)
USER_COMMON_SRC := $(wildcard libs/user/*.c)
USER_DATA_SRC   := $(wildcard libs/data/*.c)
USER_GUI_SRC    := $(wildcard libs/gui/*.c)
USER_TERM_SRC   := $(wildcard libs/term/*.c)
USER_PARSE_SRC  := libs/parse/psf.c libs/parse/ppm.c libs/parse/textdb.c

# Program set selection:
# core and ui are always built
# tools/extra/games are selectable via space separated names
# use "all" (default) or "none" for each category
USERLAND_TOOLS  ?= all
USERLAND_EXTRAS ?= all
USERLAND_GAMES  ?= all

# Helper for category selection lists such as USERLAND_TOOLS="foo bar".
USER_SELECT_DIRS = $(foreach entry,$(1),$(filter $(2)/$(entry),$(3)))

USER_CORE_PROG_DIRS := $(sort $(patsubst %/main.c, %, $(wildcard userland/core/*/main.c)))
USER_UI_PROG_DIRS := $(sort $(patsubst %/main.c, %, $(wildcard userland/ui/*/main.c)))

USER_TOOLS_ALL_PROG_DIRS := $(sort $(patsubst %/main.c, %, $(wildcard userland/tools/*/main.c)))
USER_TOOLS_ALL_NAMES := $(notdir $(USER_TOOLS_ALL_PROG_DIRS))
USER_TOOLS_OPTION_NAMES := $(USER_TOOLS_ALL_NAMES)

USER_EXTRA_ALL_PROG_DIRS := $(sort $(patsubst %/main.c, %, $(wildcard userland/extra/*/main.c)))
USER_EXTRA_ALL_NAMES := $(notdir $(USER_EXTRA_ALL_PROG_DIRS))
USER_EXTRA_OPTION_NAMES := $(USER_EXTRA_ALL_NAMES)

USER_GAMES_ALL_PROG_DIRS := $(sort $(patsubst %/main.c, %, $(wildcard userland/games/*/main.c)))
USER_GAMES_ALL_NAMES := $(notdir $(USER_GAMES_ALL_PROG_DIRS))
USER_GAMES_OPTION_NAMES := $(sort $(USER_GAMES_ALL_NAMES) doom)

ifeq ($(strip $(USERLAND_TOOLS)),none)
USER_TOOLS_PROG_DIRS :=
else ifeq ($(strip $(USERLAND_TOOLS)),all)
USER_TOOLS_PROG_DIRS := $(USER_TOOLS_ALL_PROG_DIRS)
else
USER_TOOLS_PROG_DIRS := $(call USER_SELECT_DIRS,$(USERLAND_TOOLS),userland/tools,$(USER_TOOLS_ALL_PROG_DIRS))
USER_TOOLS_UNKNOWN := $(filter-out $(USER_TOOLS_OPTION_NAMES),$(USERLAND_TOOLS))
ifneq ($(strip $(USER_TOOLS_UNKNOWN)),)
$(error Unknown tool selection(s) in USERLAND_TOOLS: $(USER_TOOLS_UNKNOWN))
endif
endif

ifeq ($(strip $(USERLAND_EXTRAS)),none)
USER_EXTRA_PROG_DIRS :=
else ifeq ($(strip $(USERLAND_EXTRAS)),all)
USER_EXTRA_PROG_DIRS := $(USER_EXTRA_ALL_PROG_DIRS)
else
USER_EXTRA_PROG_DIRS := $(call USER_SELECT_DIRS,$(USERLAND_EXTRAS),userland/extra,$(USER_EXTRA_ALL_PROG_DIRS))
USER_EXTRA_UNKNOWN := $(filter-out $(USER_EXTRA_OPTION_NAMES),$(USERLAND_EXTRAS))
ifneq ($(strip $(USER_EXTRA_UNKNOWN)),)
$(error Unknown extra selection(s) in USERLAND_EXTRAS: $(USER_EXTRA_UNKNOWN))
endif
endif

USER_ENABLE_DOOM := false
ifeq ($(strip $(USERLAND_GAMES)),none)
USER_GAMES_PROG_DIRS :=
else ifeq ($(strip $(USERLAND_GAMES)),all)
USER_GAMES_PROG_DIRS := $(USER_GAMES_ALL_PROG_DIRS)
USER_ENABLE_DOOM := true
else
USER_GAMES_PROG_DIRS := $(call USER_SELECT_DIRS,$(USERLAND_GAMES),userland/games,$(USER_GAMES_ALL_PROG_DIRS))
USER_GAMES_UNKNOWN := $(filter-out $(USER_GAMES_OPTION_NAMES),$(USERLAND_GAMES))
ifneq ($(strip $(USER_GAMES_UNKNOWN)),)
$(error Unknown game selection(s) in USERLAND_GAMES: $(USER_GAMES_UNKNOWN))
endif
ifneq ($(filter doom,$(USERLAND_GAMES)),)
USER_ENABLE_DOOM := true
endif
endif

USER_PROG_DIRS := $(sort $(USER_CORE_PROG_DIRS) $(USER_UI_PROG_DIRS) $(USER_TOOLS_PROG_DIRS) $(USER_EXTRA_PROG_DIRS) $(USER_GAMES_PROG_DIRS))
USER_APP_SRC := $(foreach prog_dir,$(USER_PROG_DIRS),$(wildcard $(prog_dir)/*.c))
USER_PROGS := $(notdir $(USER_PROG_DIRS))
USER_SDK_INCLUDE_HEADERS := \
	$(wildcard libs/libc/*.h) \
	$(wildcard libs/libc/sys/*.h) \
	$(wildcard libs/libc_usr/*.h) \
	$(wildcard libs/libc_ext/*.h)

ifneq ($(words $(USER_PROGS)), $(words $(sort $(USER_PROGS))))
$(error Duplicate userspace program names found across userland subdirectories)
endif

USER_CRT_SRC := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crt0.asm
USER_CRTI_SRC := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crti.asm
USER_CRTN_SRC := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crtn.asm


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

# ifeq ($(ARCH_VARIANT),64)
# USER_CC := $(USER_CC) -msse -msse2
# else ifeq ($(ARCH_VARIANT),32)
# USER_CC := $(USER_CC) -msse -msse2 -mfpmath=sse
# endif

USER_AS := -felf$(ARCH_VARIANT)

USER_LD := \
	--gc-sections \
	-Tuserland/linker$(ARCH_VARIANT).ld \
	$(USER_LD_EMU)


USER_LIBGCC := $(call LIBGCC, $(USER_CC))

USER_LIBC_OBJ   := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_LIBC_SRC))
USER_CRT_OBJ    := $(patsubst %.asm, $(USER_OBJ_DIR)/%.asm.o, $(USER_CRT_SRC))
USER_CRTI_OBJ   := $(patsubst %.asm, $(USER_OBJ_DIR)/%.asm.o, $(USER_CRTI_SRC))
USER_CRTN_OBJ   := $(patsubst %.asm, $(USER_OBJ_DIR)/%.asm.o, $(USER_CRTN_SRC))
USER_APP_OBJ    := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_APP_SRC))
USER_COMMON_OBJ := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_COMMON_SRC))
USER_DATA_OBJ   := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_DATA_SRC))
USER_GUI_OBJ    := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_GUI_SRC))
USER_TERM_OBJ   := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_TERM_SRC))
USER_PARSE_OBJ  := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,   $(USER_PARSE_SRC))
USER_SHARED_OBJ := $(USER_CRT_OBJ) $(USER_LIBC_OBJ) $(USER_COMMON_OBJ) \
	$(USER_DATA_OBJ) $(USER_GUI_OBJ) $(USER_TERM_OBJ) $(USER_PARSE_OBJ)

USER_SDK_OBJ_DIR := $(USER_OBJ_DIR)/sdk
USER_SDK_LIBC_A := $(USER_SDK_OBJ_DIR)/libc.a
USER_SDK_CRT1_OBJ := $(USER_SDK_OBJ_DIR)/crt1.o
USER_SDK_CRTI_OBJ := $(USER_SDK_OBJ_DIR)/crti.o
USER_SDK_CRTN_OBJ := $(USER_SDK_OBJ_DIR)/crtn.o
USER_SDK_RUNTIME_OBJ := $(USER_SDK_LIBC_A) $(USER_SDK_CRT1_OBJ) $(USER_SDK_CRTI_OBJ) $(USER_SDK_CRTN_OBJ)
USER_STAGE_USR_INCLUDE_STAMP := $(USER_STAGE_USR_INCLUDE_DIR)/.apheleia_headers
USER_STAGE_USR_LIB_STAMP := $(USER_STAGE_USR_LIB_DIR)/.apheleia_runtime

USER_PROGS_BIN := $(foreach prog, $(USER_PROGS), $(USER_BIN_DIR)/$(prog))
USER_BINARIES  := $(foreach prog, $(USER_PROGS), $(USER_STAGE_DIR)/$(prog))

DOOM_PORT_DIR := userland/games/doom
DOOM_PORT_BUNDLE_DIR := $(DOOM_PORT_DIR)/.local/build/$(ARCH)/bundle
DOOM_PORT_BIN := $(DOOM_PORT_BUNDLE_DIR)/doom
DOOM_PORT_WAD := $(DOOM_PORT_BUNDLE_DIR)/doom1.wad

USER_BINARIES += \
	$(USER_STAGE_USR_INCLUDE_STAMP) \
	$(USER_STAGE_USR_LIB_STAMP)

ifeq ($(USER_ENABLE_DOOM),true)
USER_BINARIES += \
	$(USER_STAGE_DIR)/doom \
	$(USER_STAGE_HOME_USER_DIR)/doom1.wad
endif

STRIP_USER      ?= true
USER_STRIP_FLAGS ?= --strip-debug

.SECONDARY: $(USER_SHARED_OBJ) $(USER_CRTI_OBJ) $(USER_CRTN_OBJ) $(USER_APP_OBJ) $(USER_PROGS_BIN)

$(USER_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(call cc, $(USER_CC), $@, $<)

$(USER_OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	$(call as, $(USER_AS), $@, $<)

define USER_LINK_RULE
$(USER_BIN_DIR)/$(2): $(USER_SHARED_OBJ) $$(filter $(USER_OBJ_DIR)/$(1)/%.c.o,$(USER_APP_OBJ)) $(USER_LIBGCC)
	@mkdir -p $$(@D)
	$$(call ld, $(USER_LD), $$@, $$^)
	@if [ "$(STRIP_USER)" = "true" ]; then \
		$(ST) $(USER_STRIP_FLAGS) $$@; \
	fi
endef

$(foreach prog_dir, $(USER_PROG_DIRS), \
	$(eval $(call USER_LINK_RULE,$(prog_dir),$(notdir $(prog_dir)))) \
)

$(USER_SDK_LIBC_A): $(USER_LIBC_OBJ)
	@mkdir -p $(@D)
	@rm -f $@
	@ar rcs $@ $^

$(USER_SDK_CRT1_OBJ): $(USER_CRT_OBJ)
	@mkdir -p $(@D)
	@cp $< $@

$(USER_SDK_CRTI_OBJ): $(USER_CRTI_OBJ)
	@mkdir -p $(@D)
	@cp $< $@

$(USER_SDK_CRTN_OBJ): $(USER_CRTN_OBJ)
	@mkdir -p $(@D)
	@cp $< $@

$(USER_STAGE_USR_INCLUDE_STAMP): $(USER_SDK_INCLUDE_HEADERS)
	@mkdir -p "$(USER_STAGE_USR_INCLUDE_DIR)/sys" \
		"$(USER_STAGE_USR_INCLUDE_DIR)/libc_usr" \
		"$(USER_STAGE_USR_INCLUDE_DIR)/libc_ext"
	@cp -f libs/libc/*.h "$(USER_STAGE_USR_INCLUDE_DIR)/"
	@cp -f libs/libc/sys/*.h "$(USER_STAGE_USR_INCLUDE_DIR)/sys/"
	@cp -f libs/libc_usr/*.h "$(USER_STAGE_USR_INCLUDE_DIR)/libc_usr/"
	@cp -f libs/libc_ext/*.h "$(USER_STAGE_USR_INCLUDE_DIR)/libc_ext/"
	@touch $@

$(USER_STAGE_USR_LIB_STAMP): $(USER_SDK_RUNTIME_OBJ)
	@mkdir -p "$(USER_STAGE_USR_LIB_DIR)"
	@cp "$(USER_SDK_LIBC_A)" "$(USER_STAGE_USR_LIB_DIR)/libc.a"
	@cp "$(USER_SDK_CRT1_OBJ)" "$(USER_STAGE_USR_LIB_DIR)/crt1.o"
	@cp "$(USER_SDK_CRTI_OBJ)" "$(USER_STAGE_USR_LIB_DIR)/crti.o"
	@cp "$(USER_SDK_CRTN_OBJ)" "$(USER_STAGE_USR_LIB_DIR)/crtn.o"
	@touch $@

$(DOOM_PORT_BIN) $(DOOM_PORT_WAD): $(DOOM_PORT_DIR)/Makefile $(DOOM_PORT_DIR)/doom_apheleia.c
	@$(MAKE) -C $(DOOM_PORT_DIR) bundle \
		ARCH=$(ARCH) \
		CC_BIN="$(CC)" \
		LD_BIN="$(LD)" \
		NASM_BIN="$(AS)" \
		STRIP_BIN="$(ST)" \
		VERSION="$(VERSION)" \
		BUILD_DATE="$(BUILD_DATE)" \
		GIT_COMMIT_SHORT="$(GIT_COMMIT_SHORT)"

$(USER_STAGE_DIR)/doom: $(DOOM_PORT_BIN)
	@mkdir -p $(@D)
	@cp $< $@

$(USER_STAGE_HOME_USER_DIR)/doom1.wad: $(DOOM_PORT_WAD)
	@mkdir -p $(@D)
	@cp $< $@

$(USER_STAGE_DIR)/%: $(USER_BIN_DIR)/%
	@mkdir -p $(@D)
	@cp $< $@

bin/$(IMAGE_NAME).img: $(USER_BINARIES)
bin/$(IMAGE_NAME).iso: $(USER_BINARIES)
