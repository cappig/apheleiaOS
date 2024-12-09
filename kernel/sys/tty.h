#pragma once

#include <base/types.h>
#include <boot/proto.h>
#include <gfx/state.h>
#include <term/render.h>

#define TTY_BUF_SIZE 512

// A virtual terminal is a pseudo terminal that outputs
// text to the screen and receives input from the keyboard
// One tty is always marked as the 'current tty'.
// This means that it will by default receive keyboard input and
// only and its output is rendered on screen

extern gfx_terminal* current_tty;


gfx_terminal* tty_spawn(char* name);
gfx_terminal* tty_spawn_size(char* name, usize buffer_size);

void tty_set_current(gfx_terminal* tty);

void tty_init(graphics_state* gfx_state, boot_handoff* handoff);
void tty_spawn_devs(void);
