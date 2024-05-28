#pragma once

#include "base/attributes.h"
#include "data/ring.h"
#include "vfs/fs.h"

typedef struct vfs_pty vfs_pty;

typedef isize (*vfs_pty_read_fn)(vfs_pty* term, u8* data, usize len);
typedef isize (*vfs_pty_write_fn)(vfs_pty* term, u8* data, usize len);

typedef struct vfs_pty {
    // write to input, read from output
    vfs_node* master;

    // write to output, read from input
    vfs_node* slave;

    ring_buffer* input_buffer;
    ring_buffer* output_buffer;

    vfs_pty_read_fn read;
    vfs_pty_write_fn write;

    // hook a handler that gets called on file update?
    void* private;
} vfs_pty;


vfs_pty* pty_create(usize buffer_size);
void pty_destroy(NONNULL vfs_pty* pty);
