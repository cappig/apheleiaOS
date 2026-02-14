#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <gui/input.h>
#include <input/kbd.h>
#include <input/mouse.h>
#include <poll.h>
#include <sys/vfs.h>

bool input_init(void);

bool input_capture_screen(size_t screen);

void input_push_key_event(
    const key_event* event,
    bool shift,
    bool ctrl,
    bool alt,
    bool capslock
);
void input_push_mouse_event(const mouse_event* event);

ssize_t input_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags);
short input_poll(vfs_node_t* node, short events, u32 flags);
