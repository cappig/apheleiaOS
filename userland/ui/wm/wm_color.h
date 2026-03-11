#pragma once

#include <base/types.h>
#include <stdbool.h>

bool wm_parse_hex_color(const char *text, u32 *color_out);
