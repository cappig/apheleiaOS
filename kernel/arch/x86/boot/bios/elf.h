#pragma once

#include <base/types.h>
#include <x86/boot.h>

void load_kerenel(boot_info_t *info);

void jump_to_kernel_32(u32 entry, u32 boot_info, u32 stack);
void jump_to_kernel_64(u64 entry, u64 boot_info, u64 stack);
