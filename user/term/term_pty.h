#pragma once

#include <base/types.h>
#include <gui/ws.h>
#include <sys/types.h>

pid_t term_spawn_shell(int master_fd, size_t cols, size_t rows, u32 width, u32 height);
void term_handle_key_event(int master_fd, const ws_input_event_t *event);
