#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <gfx/state.h>
#include <log/log.h>
#include <term/term.h>
#include <x86/asm.h>

#define panic(...)              \
    ({                          \
        log_fatal(__VA_ARGS__); \
        halt();                 \
    })


void puts(const char* s);
void serial_puts(const char* s);

void tty_init(graphics_state* gfx_ptr);
