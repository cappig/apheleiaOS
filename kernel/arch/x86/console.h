#pragma once

#include <sys/font.h>
#include <x86/boot.h>

void console_init(const boot_info_t* info);
void console_set_font(const font_t* font);
