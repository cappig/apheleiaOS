USER_OBJS += $(USER_BIN)/vdso.elf


VDSO_SRC := \
	$(wildcard user/vdso/*.asm)

VDSO_OBJ := \
	$(patsubst %, bin/%.o, $(VDSO_SRC))


$(USER_BIN)/vdso.elf: $(VDSO_OBJ)
	@mkdir -p $(@D)
	$(LD) $(LD_USER) -shared -o $@ $^
	$(ST) $@
