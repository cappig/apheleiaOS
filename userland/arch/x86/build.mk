USER_CRT_SRC   := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crt0.asm
USER_CRTI_SRC  := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crti.asm
USER_CRTN_SRC  := libs/libc_usr/arch/x86_$(ARCH_VARIANT)/crtn.asm
USER_LD_SCRIPT := userland/arch/x86/linker$(ARCH_VARIANT).ld

USER_AS := -felf$(ARCH_VARIANT)

ifeq ($(ARCH_VARIANT),64)
USER_ARCH_CFLAGS := -m64
USER_LD_EMU      := -melf_x86_64
TCC_TARGET_NAME    := x86_64
TCC_TARGET_DEFS    := -DTCC_TARGET_X86_64
TCC_TARGET_TRIPLET := x86_64-apheleia
else ifeq ($(ARCH_VARIANT),32)
USER_ARCH_CFLAGS := -m32
USER_LD_EMU      := -melf_i386
TCC_TARGET_NAME    := i386
TCC_TARGET_DEFS    := -DTCC_TARGET_I386
TCC_TARGET_TRIPLET := i386-apheleia
else
$(error Unsupported x86 variant '$(ARCH_VARIANT)')
endif
