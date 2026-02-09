#pragma once

#include <base/types.h>
#include <input/mouse.h>
#include <sys/vfs.h>

#define MOUSE_DEV_BUFFER_SIZE 256

typedef struct {
    const char* name;
} mouse_dev_t;

void mouse_handle_event(mouse_event event);

u8 mouse_register(const char* name);

bool mouse_init(void);

ssize_t mouse_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags);
