SHELL := /bin/sh

TCC_PORT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ROOT_DIR ?= $(abspath $(TCC_PORT_DIR)/../../..)

TCC_REPO ?= https://github.com/sellicott/tcc-riscv32.git
TCC_REF  ?= de6f179e17d804541d6984bb9f3237c7ec9a6b78
TCC_SRC  ?= $(TCC_PORT_DIR)/.local/upstream
TCC_MAKEFILE := $(TCC_PORT_DIR)/Makefile
TCC_PATCH    := $(TCC_PORT_DIR)/patches/riscv32-relocs.patch

TCC_BUILD_DIR := $(TCC_PORT_DIR)/.local/build/$(ARCH)
TCC_OBJ_DIR   := $(TCC_BUILD_DIR)/obj
TCC_BIN_DIR   := $(TCC_BUILD_DIR)/bin

TCC_SYNC_STAMP       := $(TCC_SRC)/.apheleia_sync
TCC_PATCH_STAMP      := $(TCC_SRC)/.apheleia_patch
TCC_CONFIG_H         := $(TCC_BUILD_DIR)/config.h
TCC_MAIN_C           := $(TCC_BUILD_DIR)/apheleia_tcc.c
TCC_C2STR            := $(TCC_BUILD_DIR)/c2str
TCC_DEFS_H           := $(TCC_BUILD_DIR)/tccdefs_.h
TCC_OBJ              := $(TCC_OBJ_DIR)/tcc.o
TCC_BIN              := $(TCC_BIN_DIR)/tcc
TCC_STAGE_LIB_DIR    := $(USER_STAGE_USR_LIB_DIR)/tcc
TCC_STAGE_INC_STAMP  := $(USER_STAGE_STAMP_DIR)/tcc-include

HOST_CC ?= cc

ifeq ($(ARCH),x86_32)
TCC_TARGET_NAME := i386
TCC_TARGET_DEFS := -DTCC_TARGET_I386
TCC_TARGET_TRIPLET := i386-apheleia
else ifeq ($(ARCH),x86_64)
TCC_TARGET_NAME := x86_64
TCC_TARGET_DEFS := -DTCC_TARGET_X86_64
TCC_TARGET_TRIPLET := x86_64-apheleia
else ifeq ($(ARCH),riscv_32)
TCC_TARGET_NAME := riscv32
TCC_TARGET_DEFS := -DTCC_TARGET_RISCV32 -DTCC_RISCV_ilp32
TCC_TARGET_TRIPLET := riscv32-apheleia
else ifeq ($(ARCH),riscv_64)
TCC_TARGET_NAME := riscv64
TCC_TARGET_DEFS := -DTCC_TARGET_RISCV64
TCC_TARGET_TRIPLET := riscv64-apheleia
else
$(error TCC does not know how to target ARCH=$(ARCH))
endif

TCC_CFLAGS := \
	$(CC_BASE) \
	$(USER_CC) \
	$(TCC_TARGET_DEFS) \
	-DONE_SOURCE=1 \
	-DCONFIG_TCC_STATIC \
	-DCONFIG_TCC_BACKTRACE=0 \
	-DCONFIG_TCC_BCHECK=0 \
	-DCONFIG_TCC_SEMLOCK=0 \
	-DTCC_LIBTCC1=\"\" \
	-I$(TCC_BUILD_DIR) \
	-I$(TCC_SRC) \
	-I$(TCC_SRC)/include \
	-Wno-declaration-after-statement \
	-Wno-format \
	-Wno-implicit-fallthrough \
	-Wno-missing-field-initializers \
	-Wno-shadow \
	-Wno-sign-compare \
	-Wno-unused-function \
	-Wno-unused-parameter \
	-Wno-unused-variable

ifeq ($(TOOLCHAIN),llvm)
TCC_CFLAGS += \
	-Wno-excess-initializers \
	-Wno-unterminated-string-initialization
endif

USER_BINARIES += $(USER_STAGE_DIR)/tcc $(TCC_STAGE_INC_STAMP)

$(TCC_SYNC_STAMP):
	@mkdir -p "$(dir $(TCC_SRC))"
	@if [ ! -d "$(TCC_SRC)/.git" ]; then \
		git clone --depth 1 "$(TCC_REPO)" "$(TCC_SRC)"; \
	fi
	@current="$$(git -C "$(TCC_SRC)" rev-parse HEAD 2>/dev/null || echo unknown)"; \
	if [ "$$current" != "$(TCC_REF)" ]; then \
		git -C "$(TCC_SRC)" fetch --depth 1 origin "$(TCC_REF)"; \
		git -C "$(TCC_SRC)" checkout --detach "$(TCC_REF)"; \
	fi
	@touch "$@"

$(TCC_PATCH_STAMP): $(TCC_SYNC_STAMP) $(TCC_PATCH) $(TCC_MAKEFILE)
	@git -C "$(TCC_SRC)" checkout -- .
	@git -C "$(TCC_SRC)" apply "$(TCC_PATCH)"
	@touch "$@"

$(TCC_CONFIG_H): $(TCC_PATCH_STAMP) $(TCC_MAKEFILE)
	@mkdir -p "$(@D)"
	@{ \
		echo "/* Generated for apheleiaOS. */"; \
		echo "#define TCC_VERSION \"0.9.28-rv32\""; \
		echo "#define CC_NAME CC_gcc"; \
		echo "#define GCC_MAJOR 0"; \
		echo "#define GCC_MINOR 0"; \
		echo "#define CONFIG_TCC_SYSINCLUDEPATHS \"/usr/include:/usr/lib/tcc/include\""; \
		echo "#define CONFIG_TCC_LIBPATHS \"/usr/lib\""; \
		echo "#define CONFIG_TCC_CRTPREFIX \"/usr/lib\""; \
		echo "#define CONFIG_TCCDIR \"/usr/lib/tcc\""; \
		echo "#define CONFIG_TCC_TARGET \"$(TCC_TARGET_NAME)\""; \
		echo "#define CONFIG_TRIPLET \"$(TCC_TARGET_TRIPLET)\""; \
		echo "#define CONFIG_TCC_PREDEFS 1"; \
	} > "$@"

$(TCC_C2STR): $(TCC_PATCH_STAMP)
	@mkdir -p "$(@D)"
	@$(HOST_CC) -DC2STR "$(TCC_SRC)/conftest.c" -o "$@"

$(TCC_DEFS_H): $(TCC_C2STR) $(TCC_PATCH_STAMP)
	@mkdir -p "$(@D)"
	@"$(TCC_C2STR)" "$(TCC_SRC)/include/tccdefs.h" "$@"

$(TCC_MAIN_C): $(TCC_PATCH_STAMP) $(TCC_MAKEFILE)
	@mkdir -p "$(@D)"
	@{ \
		echo "#include \"tcc.c\""; \
	} > "$@"

$(TCC_OBJ): $(TCC_MAIN_C) $(TCC_CONFIG_H) $(TCC_DEFS_H) \
	$(USER_COMPILE_STAMP) $(TCC_MAKEFILE)
	@mkdir -p "$(@D)"
	$(CC) $(TCC_CFLAGS) -c "$(TCC_MAIN_C)" -o "$@"

$(TCC_BIN): $(USER_SHARED_OBJ) $(USER_LIBGCC) $(TCC_OBJ) \
	$(USER_LD_SCRIPT) $(USER_LINK_STAMP)
	@mkdir -p "$(@D)"
	$(call ld, $(USER_LD), $@, $(filter-out $(USER_LD_SCRIPT) $(USER_LINK_STAMP) $(USER_LIBGCC),$^) $(USER_LIBGCC))
	$(call user_strip, $@)
	@printf "%-3s  %s (%s)\n" "TCC" "$@" "$(TCC_TARGET_NAME)"

$(USER_STAGE_DIR)/tcc: $(TCC_BIN) | $(USER_STAGE_BIN_STAMP)
	@mkdir -p "$(@D)"
	@cp -f "$<" "$@"

$(TCC_STAGE_INC_STAMP): $(TCC_PATCH_STAMP)
	@mkdir -p "$(@D)" "$(TCC_STAGE_LIB_DIR)/include"
	@cp -f "$(TCC_SRC)"/include/*.h "$(TCC_STAGE_LIB_DIR)/include/"
	@cp -f "$(TCC_SRC)/tcclib.h" "$(TCC_STAGE_LIB_DIR)/include/"
	@touch "$@"
