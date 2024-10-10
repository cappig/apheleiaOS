#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <boot/proto.h>
#include <gfx/psf.h>
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

#define ALPHA_COLORS_DIM               \
    "\e[40m  \e[41m  \e[42m  \e[43m  " \
    "\e[44m  \e[45m  \e[46m  \e[47;0m"

#define ALPHA_COLORS_BRIGHT                \
    "\e[100m  \e[101m  \e[102m  \e[103m  " \
    "\e[104m  \e[105m  \e[106m  \e[107;0m"

#define ALPHA_BUILD_DATE "Built on " __DATE__ " at " __TIME__

typedef struct {
    graphics_state gfx;

    terminal* term;

    psf_font font;
} kernel_console;


void puts(const char* s);

void tty_init(graphics_state* gfx_ptr, boot_handoff* handoff);
