#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <gfx/state.h>
#include <log/log.h>
#include <term/term.h>

#define ALPHA_ASCII                     \
    "\n"                                \
    " _________                     \n" \
    "  @-----@     The Apheleia     \n" \
    "   | | |    operating system   \n" \
    "   | | |                       \n" \
    "   | | |    (c) 2024 - GPLv3   \n" \
    "   | | |                     \n\n"

void puts(const char* s);
void serial_puts(const char* s);

void tty_init(graphics_state* gfx_ptr);
