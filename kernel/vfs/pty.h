#pragma once

#include <data/ring.h>
#include <data/vector.h>

#include "fs.h"

#define PTY_RETURN_CHAR '\n'
#define PTY_DELETE_CHAR '\b'

typedef struct pseudo_tty pseudo_tty;

typedef void (*vfs_hook_fn)(pseudo_tty* term, void* buf, usize len);

typedef struct pseudo_tty {
    // write to input, read from output
    vfs_node* master;

    // write to output, read from input
    vfs_node* slave;

    ring_buffer* input_buffer;
    ring_buffer* output_buffer;

    vector* line_buffer; // Used by canonical mode

    u8 flags;

    // Hooks that can be called when there is new data in the buffer, optional
    vfs_hook_fn out_hook; // There is new data in the output_buffer, the master can read
    vfs_hook_fn in_hook; // There is new data in the input_buffer, the slave can read

    void* private;
} pseudo_tty;

enum pty_flags {
    PTY_ECHO = 1 << 0,
    PTY_CANONICAL = 1 << 1,
    PTY_SIGNAL = 1 << 2,
};


pseudo_tty* pty_create(usize buffer_size);

void pty_destroy(pseudo_tty* pty);
