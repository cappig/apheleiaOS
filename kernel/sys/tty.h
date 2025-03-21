#pragma once

#include <base/types.h>
#include <boot/proto.h>
#include <gfx/state.h>
#include <term/render.h>

#include "vfs/pty.h"

#define TTY_COUNT   4
#define TTY_CONSOLE (TTY_COUNT - 1)

#define TTY_BUF_SIZE 512

#define TTY_NONE (-1)

// A virtual terminal is a pseudo terminal that outputs
// text to the screen and receives input from the keyboard
// One tty is always marked as the 'current tty'.
// This means that it will by default receive keyboard input and
// only and its output is rendered on screen

typedef struct {
    usize id;
    pseudo_tty* pty;
    gfx_terminal* gterm;
} virtual_tty;

extern isize current_tty;


void tty_input(usize index, u8* data, usize len);
void tty_output(usize index, u8* data, usize len);

bool tty_set_current(usize index);
virtual_tty* get_tty(isize index);

virtual_tty* tty_spawn(void);
void tty_spawn_devs(void);

void tty_init(boot_handoff* handoff);
