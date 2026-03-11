#pragma once

#include <base/types.h>
#include <input/mouse.h>

bool mouse_init(void);
u8 mouse_register(const char *name);
void mouse_handle_event(mouse_event event);
