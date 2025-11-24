#pragma once

#include <base/types.h>
#include <x86/lib/regs.h>


void bios_call(u8 number, regs* in_regs, regs* out_regs);
