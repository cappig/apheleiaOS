#pragma once

#include <data/ring.h>

#include "vfs/fs.h"

typedef struct vfs_pty vfs_pty;

typedef void (*vfs_hook_fn)(vfs_pty* term, void* buf, usize len);

typedef struct vfs_pty {
    // write to input, read from output
    vfs_node* master;

    // write to output, read from input
    vfs_node* slave;

    ring_buffer* input_buffer;
    ring_buffer* output_buffer;

    // Hooks that can be called when there is new data in the buffer, optional
    vfs_hook_fn out_hook; // There is new data in the output_buffer, the master can read
    vfs_hook_fn in_hook; // There is new data in the input_buffer, the slave can read

    void* private;
} vfs_pty;


vfs_pty* pty_create(usize buffer_size);

void pty_destroy(vfs_pty* pty);
