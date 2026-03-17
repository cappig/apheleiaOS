SHELL := /bin/sh

DOOM_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ROOT_DIR ?= $(abspath $(DOOM_DIR)/../../..)

UPSTREAM_REPO  ?= https://github.com/ozkl/doomgeneric.git
UPSTREAM_REF   ?= fc601639494e089702a1ada082eb51aaafc03722
UPSTREAM_DIR   ?= $(DOOM_DIR)/.local/upstream
UPSTREAM_STAMP := $(UPSTREAM_DIR)/.apheleia_sync

WAD_URL   ?= https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad
WAD_SHA1  ?= 5b2e249b9c5133ec987b3ea77596381dc0d6bc1d
ASSET_DIR ?= $(DOOM_DIR)/.local/assets
WAD_FILE  := $(ASSET_DIR)/doom1.wad

BUILD_DIR  := $(DOOM_DIR)/.local/build/$(ARCH)
OBJ_DIR    := $(BUILD_DIR)/obj
BIN_DIR    := $(BUILD_DIR)/bin
BUNDLE_DIR := $(BUILD_DIR)/bundle

OUT_BIN        := $(BIN_DIR)/doom
OUT_BUNDLE_BIN := $(BUNDLE_DIR)/doom
OUT_BUNDLE_WAD := $(BUNDLE_DIR)/doom1.wad

DOOM_CC := \
	-DNORMALUNIX \
	-DLINUX \
	-DSNDSERV \
	-D_DEFAULT_SOURCE \
	-I$(UPSTREAM_DIR)/doomgeneric \
	-I$(DOOM_DIR)

CFLAGS_SUPPORT := $(DOOM_CC) $(CC_BASE) -std=gnu2x $(USER_CC)
CFLAGS_DOOM    := $(DOOM_CC) $(CC_BASE) -std=gnu17 -Wno-implicit-function-declaration $(USER_CC)
LDFLAGS        := $(LD_BASE) $(USER_LD)
ASFLAGS        := $(AS_BASE) $(USER_AS)

SUPPORT_SRC := \
	$(wildcard $(ROOT_DIR)/libs/libc/*.c) \
	$(wildcard $(ROOT_DIR)/libs/libc_ext/*.c) \
	$(wildcard $(ROOT_DIR)/libs/libc_usr/*.c) \
	$(wildcard $(ROOT_DIR)/libs/user/*.c) \
	$(wildcard $(ROOT_DIR)/libs/data/*.c) \
	$(wildcard $(ROOT_DIR)/libs/gui/*.c) \
	$(wildcard $(ROOT_DIR)/libs/term/*.c) \
	$(ROOT_DIR)/libs/parse/psf.c \
	$(ROOT_DIR)/libs/parse/ppm.c \
	$(ROOT_DIR)/libs/parse/textdb.c

SUPPORT_OBJ := $(patsubst $(ROOT_DIR)/%.c,$(OBJ_DIR)/root/%.o,$(SUPPORT_SRC))

DOOM_SRC_UPSTREAM_ALL = $(wildcard $(UPSTREAM_DIR)/doomgeneric/*.c)
DOOM_SRC_EXCLUDE = \
	$(UPSTREAM_DIR)/doomgeneric/doomgeneric_%.c \
	$(UPSTREAM_DIR)/doomgeneric/i_allegro%.c \
	$(UPSTREAM_DIR)/doomgeneric/i_sdl%.c \
	$(UPSTREAM_DIR)/doomgeneric/gusconf.c \
	$(UPSTREAM_DIR)/doomgeneric/icon.c \
	$(UPSTREAM_DIR)/doomgeneric/mus2mid.c
DOOM_SRC_UPSTREAM = $(filter-out $(DOOM_SRC_EXCLUDE),$(DOOM_SRC_UPSTREAM_ALL))

DOOM_OBJ = \
	$(patsubst $(UPSTREAM_DIR)/doomgeneric/%.c,$(OBJ_DIR)/upstream/%.o,$(DOOM_SRC_UPSTREAM)) \
	$(OBJ_DIR)/port/doom_apheleia.o

CRT_OBJ := $(OBJ_DIR)/crt0.o

USER_BINARIES += $(USER_STAGE_DIR)/doom $(USER_STAGE_HOME_USER_DIR)/doom1.wad

$(UPSTREAM_STAMP):
	@mkdir -p "$(dir $(UPSTREAM_DIR))"
	@if [ ! -d "$(UPSTREAM_DIR)/.git" ]; then \
		git clone --depth 1 "$(UPSTREAM_REPO)" "$(UPSTREAM_DIR)"; \
	fi
	@current="$$(git -C "$(UPSTREAM_DIR)" rev-parse HEAD 2>/dev/null || echo unknown)"; \
	if [ "$$current" != "$(UPSTREAM_REF)" ]; then \
		git -C "$(UPSTREAM_DIR)" fetch --depth 1 origin "$(UPSTREAM_REF)"; \
		git -C "$(UPSTREAM_DIR)" checkout --detach "$(UPSTREAM_REF)"; \
	fi
	@touch "$@"


$(WAD_FILE):
	@mkdir -p "$(ASSET_DIR)"
	@curl -L --fail --retry 3 --retry-delay 1 -o "$@" "$(WAD_URL)"
	@printf "%s  %s\n" "$(WAD_SHA1)" "$@" | sha1sum -c -

$(SUPPORT_OBJ): | $(UPSTREAM_STAMP)

$(OBJ_DIR)/root/%.o: $(ROOT_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_SUPPORT) -c $< -o $@

$(UPSTREAM_DIR)/doomgeneric/%.c: | $(UPSTREAM_STAMP)
	@:

$(OBJ_DIR)/upstream/%.o: $(UPSTREAM_DIR)/doomgeneric/%.c | $(UPSTREAM_STAMP)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_DOOM) -c $< -o $@

$(OBJ_DIR)/port/%.o: $(DOOM_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_DOOM) -c $< -o $@

$(CRT_OBJ): $(USER_CRT_SRC)
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) -o $@ $<

ifeq ($(DOOM_RECURSE),1)
.SECONDEXPANSION:
$(OUT_BIN): $(SUPPORT_OBJ) $$(DOOM_OBJ) $(CRT_OBJ) $(USER_LIBGCC) | $(UPSTREAM_STAMP)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -o $@ $^
	@if [ "$(STRIP_USER)" = "true" ]; then \
		$(ST) $(USER_STRIP_FLAGS) $@; \
	fi
else
$(OUT_BIN): | $(UPSTREAM_STAMP)
	@$(MAKE) --no-print-directory DOOM_RECURSE=1 $(OUT_BIN)
endif

$(OUT_BUNDLE_BIN): $(OUT_BIN)
	@mkdir -p "$(BUNDLE_DIR)"
	@cp -f "$(OUT_BIN)" "$(OUT_BUNDLE_BIN)"

$(OUT_BUNDLE_WAD): $(WAD_FILE)
	@mkdir -p "$(BUNDLE_DIR)"
	@cp -f "$(WAD_FILE)" "$(OUT_BUNDLE_WAD)"

$(USER_STAGE_DIR)/doom: $(OUT_BUNDLE_BIN)
	@mkdir -p $(@D)
	@cp -f $< $@

$(USER_STAGE_HOME_USER_DIR)/doom1.wad: $(OUT_BUNDLE_WAD)
	@mkdir -p $(@D)
	@cp -f $< $@
