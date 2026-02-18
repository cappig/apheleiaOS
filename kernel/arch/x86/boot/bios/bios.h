#pragma once

#include <base/types.h>
#include <x86/regs.h>


void bios_call(u8 number, regs32_t *in_regs, regs32_t *out_regs);
