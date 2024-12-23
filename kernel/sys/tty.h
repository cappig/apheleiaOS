#pragma once

#include <base/types.h>
#include <boot/proto.h>
#include <gfx/state.h>
#include <term/render.h>

#include "vfs/pty.h"

#define TTY_BUF_SIZE 512

// A virtual terminal is a pseudo terminal that outputs
// text to the screen and receives input from the keyboard
// One tty is always marked as the 'current tty'.
// This means that it will by default receive keyboard input and
// only and its output is rendered on screen

extern pseudo_tty* current_tty;


pseudo_tty* tty_spawn(char* name);
pseudo_tty* tty_spawn_sized(char* name, usize buffer_size);

bool tty_set_current(pseudo_tty* pty);

void tty_current_input(u8 data);

void tty_init(graphics_state* gfx_state, boot_handoff* handoff);
void tty_spawn_devs(void);
