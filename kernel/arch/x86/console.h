#pragma once

#include <x86/boot.h>

typedef struct console_font console_font_t;

void console_init(const boot_info_t* info);
void console_set_font(const console_font_t* font);
