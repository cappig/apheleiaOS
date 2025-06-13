USER_OBJS += $(USER_BIN)/sh.elf


SH_SRC := \
	$(wildcard user/sh/*.c) \
	$(addprefix user/, $(ULIB_SRC))

SH_OBJ := \
	$(patsubst %, bin/%.o, $(SH_SRC))


$(USER_BIN)/sh.elf: $(SH_OBJ)
	@mkdir -p $(@D)
	$(call ld, $(LD_USER), $@, $^)
