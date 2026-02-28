SHELL := /bin/sh

ROOT_DIR := $(abspath ../../..)

UPSTREAM_REPO ?= https://github.com/ozkl/doomgeneric.git
UPSTREAM_REF ?= fc601639494e089702a1ada082eb51aaafc03722
UPSTREAM_DIR ?= $(CURDIR)/.local/upstream
UPSTREAM_STAMP := $(UPSTREAM_DIR)/.apheleia_sync

WAD_URL ?= https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad
WAD_SHA1 ?= 5b2e249b9c5133ec987b3ea77596381dc0d6bc1d
ASSET_DIR ?= $(CURDIR)/.local/assets
WAD_FILE := $(ASSET_DIR)/doom1.wad

BUILD_DIR ?= $(CURDIR)/.local/build/$(ARCH)
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
BUNDLE_DIR := $(BUILD_DIR)/bundle
OUT_BIN := $(BIN_DIR)/doom
OUT_BUNDLE_BIN := $(BUNDLE_DIR)/doom
OUT_BUNDLE_WAD := $(BUNDLE_DIR)/doom1.wad

ARCH ?= x86_64

ifeq ($(ARCH),x86_64)
ARCH_VARIANT := 64
USER_ARCH_NAME := x86_64
USER_LD_EMU := -melf_x86_64
USER_CRT_SRC := $(ROOT_DIR)/libs/libc_usr/arch/x86_64/crt0.asm
USER_LINKER_SCRIPT := $(ROOT_DIR)/userland/linker64.ld
else ifeq ($(ARCH),x86_32)
ARCH_VARIANT := 32
USER_ARCH_NAME := x86_32
USER_LD_EMU := -melf_i386
USER_CRT_SRC := $(ROOT_DIR)/libs/libc_usr/arch/x86_32/crt0.asm
USER_LINKER_SCRIPT := $(ROOT_DIR)/userland/linker32.ld
else
$(error Unsupported ARCH '$(ARCH)')
endif

CC_BIN ?= x86_64-linux-gnu-gcc
LD_BIN ?= ld
NASM_BIN ?= nasm
STRIP_BIN ?= strip

BUILD_DATE ?= $(shell date -u +%Y-%m-%d)
GIT_COMMIT_SHORT ?= $(shell git -C "$(ROOT_DIR)" rev-parse --short HEAD 2>/dev/null || echo unknown)
VERSION ?= alpha-0.2

CC_BASE := \
	-MD \
	-Wall \
	-Wextra \
	-Wshadow \
	-I$(ROOT_DIR)/libs \
	-I$(ROOT_DIR)/libs/libc \
	-I$(ROOT_DIR)/kernel \
	-I$(ROOT_DIR)/kernel/arch \
	-ffreestanding \
	-fno-stack-protector \
	-DEXTEND_LIBC \
	-D_APHELEIA_SOURCE \
	-nostdinc \
	-mno-red-zone \
	-fno-pic \
	-fno-pie \
	-Wno-unused-parameter \
	-Wno-missing-braces \
	-DVERSION=\"$(VERSION)\" \
	-DBUILD_DATE=\"$(BUILD_DATE)\" \
	-DGIT_COMMIT=\"$(GIT_COMMIT_SHORT)\" \
	-g \
	-fno-omit-frame-pointer \
	-O3

USER_CC := \
	-nostdlib \
	-I$(ROOT_DIR)/libs/libc_usr \
	-I$(ROOT_DIR)/libs/user \
	-I$(ROOT_DIR)/libs/gui \
	-m$(ARCH_VARIANT) \
	-DARCH_NAME=\"$(USER_ARCH_NAME)\"

DOOM_CC := \
	-DNORMALUNIX \
	-DLINUX \
	-DSNDSERV \
	-D_DEFAULT_SOURCE \
	-I$(UPSTREAM_DIR)/doomgeneric \
	-I$(CURDIR)

CFLAGS_SUPPORT := $(DOOM_CC) $(CC_BASE) -std=gnu2x $(USER_CC)
CFLAGS_DOOM := $(DOOM_CC) $(CC_BASE) -std=gnu17 -Wno-implicit-function-declaration $(USER_CC)
LDFLAGS := -z noexecstack --gc-sections -T$(USER_LINKER_SCRIPT) $(USER_LD_EMU)
ASFLAGS := -felf$(ARCH_VARIANT)

# DOOM_OPT_FLAGS := -O3
# ifeq ($(ARCH),x86_32)
# DOOM_OPT_FLAGS := -O2
# endif

CFLAGS_SUPPORT += $(DOOM_OPT_FLAGS)
CFLAGS_DOOM += $(DOOM_OPT_FLAGS)

LIBC_SRC := $(wildcard $(ROOT_DIR)/libs/libc/*.c) $(wildcard $(ROOT_DIR)/libs/libc_ext/*.c) \
	$(wildcard $(ROOT_DIR)/libs/libc_usr/*.c)
COMMON_SRC := $(wildcard $(ROOT_DIR)/libs/user/*.c)
DATA_SRC := $(wildcard $(ROOT_DIR)/libs/data/*.c)
GUI_SRC := $(wildcard $(ROOT_DIR)/libs/gui/*.c)
TERM_SRC := $(wildcard $(ROOT_DIR)/libs/term/*.c)
PARSE_SRC := $(ROOT_DIR)/libs/parse/psf.c $(ROOT_DIR)/libs/parse/ppm.c $(ROOT_DIR)/libs/parse/textdb.c
SUPPORT_SRC := $(LIBC_SRC) $(COMMON_SRC) $(DATA_SRC) $(GUI_SRC) $(TERM_SRC) $(PARSE_SRC)

.PHONY: all fetch build bundle clean distclean print-config

all: bundle

fetch: $(UPSTREAM_STAMP) $(WAD_FILE)

build: $(OUT_BIN)

bundle: $(OUT_BUNDLE_BIN) $(OUT_BUNDLE_WAD)

print-config:
	@echo ROOT_DIR=$(ROOT_DIR)
	@echo UPSTREAM_DIR=$(UPSTREAM_DIR)
	@echo UPSTREAM_REF=$(UPSTREAM_REF)
	@echo WAD_FILE=$(WAD_FILE)
	@echo OUT_BIN=$(OUT_BIN)
	@echo OUT_BUNDLE_BIN=$(OUT_BUNDLE_BIN)
	@echo CC_BIN=$(CC_BIN)
	@echo LD_BIN=$(LD_BIN)
	@echo NASM_BIN=$(NASM_BIN)

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

$(OUT_BIN): $(UPSTREAM_STAMP) $(WAD_FILE) $(CURDIR)/doom_apheleia.c $(SUPPORT_SRC) $(USER_CRT_SRC) $(USER_LINKER_SCRIPT)
	@set -eu; \
	mkdir -p "$(OBJ_DIR)" "$(BIN_DIR)"; \
	objs=""; \
	for src in $(SUPPORT_SRC); do \
		rel="$${src#$(ROOT_DIR)/}"; \
		obj="$(OBJ_DIR)/root/$${rel%.c}.o"; \
		mkdir -p "$$(dirname "$$obj")"; \
		if ! $(CC_BIN) $(CFLAGS_SUPPORT) -c "$$src" -o "$$obj"; then \
			status=$$?; \
			if [ "$$status" -eq 135 ]; then \
				echo "doom: retrying $$src with safer optimization flags" >&2; \
				$(CC_BIN) $(CFLAGS_SUPPORT) -O1 -c "$$src" -o "$$obj"; \
			else \
				exit "$$status"; \
			fi; \
		fi; \
		objs="$$objs $$obj"; \
	done; \
	doom_objs="$$(awk '/^SRC_DOOM[[:space:]]*=/{sub(/^[^=]*=[[:space:]]*/, ""); print; exit}' "$(UPSTREAM_DIR)/doomgeneric/Makefile")"; \
	for obj_name in $$doom_objs; do \
		base="$${obj_name%.o}"; \
		if [ "$$base" = "doomgeneric_xlib" ]; then \
			src="$(CURDIR)/doom_apheleia.c"; \
			obj="$(OBJ_DIR)/port/doom_apheleia.o"; \
		else \
			src="$(UPSTREAM_DIR)/doomgeneric/$$base.c"; \
			obj="$(OBJ_DIR)/upstream/$$base.o"; \
		fi; \
		mkdir -p "$$(dirname "$$obj")"; \
		if ! $(CC_BIN) $(CFLAGS_DOOM) -c "$$src" -o "$$obj"; then \
			status=$$?; \
			if [ "$$status" -eq 135 ]; then \
				echo "doom: retrying $$src with safer optimization flags" >&2; \
				$(CC_BIN) $(CFLAGS_DOOM) -O1 -c "$$src" -o "$$obj"; \
			else \
				exit "$$status"; \
			fi; \
		fi; \
		objs="$$objs $$obj"; \
	done; \
	crt_obj="$(OBJ_DIR)/crt0.o"; \
	$(NASM_BIN) $(ASFLAGS) -o "$$crt_obj" "$(USER_CRT_SRC)"; \
	objs="$$objs $$crt_obj"; \
	libgcc="$$( $(CC_BIN) $(CC_BASE) $(USER_CC) -print-libgcc-file-name )"; \
	if ! $(LD_BIN) $(LDFLAGS) -o "$(OUT_BIN)" $$objs "$$libgcc"; then \
		status=$$?; \
		if [ "$$status" -eq 135 ]; then \
			echo "doom: retrying linker after toolchain signal" >&2; \
			$(LD_BIN) $(LDFLAGS) -o "$(OUT_BIN)" $$objs "$$libgcc"; \
		else \
			exit "$$status"; \
		fi; \
	fi; \
	if [ "$${STRIP_OUTPUT:-true}" = "true" ]; then \
		if ! $(STRIP_BIN) --strip-debug "$(OUT_BIN)"; then \
			status=$$?; \
			if [ "$$status" -eq 135 ]; then \
				echo "doom: retrying strip after toolchain signal" >&2; \
				$(STRIP_BIN) --strip-debug "$(OUT_BIN)"; \
			else \
				exit "$$status"; \
			fi; \
		fi; \
	fi

$(OUT_BUNDLE_BIN): $(OUT_BIN)
	@mkdir -p "$(BUNDLE_DIR)"
	@cp -f "$(OUT_BIN)" "$(OUT_BUNDLE_BIN)"

$(OUT_BUNDLE_WAD): $(WAD_FILE)
	@mkdir -p "$(BUNDLE_DIR)"
	@cp -f "$(WAD_FILE)" "$(OUT_BUNDLE_WAD)"

clean:
	@rm -rf "$(BUILD_DIR)"

distclean: clean
	@rm -rf "$(UPSTREAM_DIR)" "$(ASSET_DIR)"
