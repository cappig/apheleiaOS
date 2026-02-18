#pragma once

#include <base/types.h>
#include <input/kbd.h>
#include <input/keymap.h>
#include <sys/vfs.h>

#define KBD_DEV_BUFFER_SIZE 256

typedef struct {
    const char *name;

    bool shift;
    bool ctrl;
    bool alt;
    bool capslock;

    ascii_keymap *keymap;
} keyboard_dev_t;

bool keyboard_init(void);

u8 keyboard_register(const char *name, ascii_keymap *keymap);

void keyboard_handle_key(key_event event);

ssize_t keyboard_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags);
