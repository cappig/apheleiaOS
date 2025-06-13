USER_OBJS += $(USER_BIN)/init.elf


INIT_SRC := \
	$(wildcard user/init/*.c) \
	$(addprefix user/, $(ULIB_SRC))

INIT_OBJ := \
	$(patsubst %, bin/%.o, $(INIT_SRC))


$(USER_BIN)/init.elf: $(INIT_OBJ)
	@mkdir -p $(@D)
	$(call ld, $(LD_USER), $@, $^)
#	$(call st, $@)
