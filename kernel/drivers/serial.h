#pragma once

#include <base/types.h>
#include <data/ring.h>

#include "vfs/pty.h"

#define SERIAL_DEV_BUFFER_SIZE 256


void serial_output(usize port, u8* data, usize len);

bool pty_hook_serial(pseudo_tty* pty, usize port);

bool init_serial_port(u8 index);

void init_serial_dev(void);
