#include "pty.h"

#include <data/ring.h>
#include <data/vector.h>

#include "base/types.h"
#include "mem/heap.h"
#include "vfs/fs.h"

// A pseudo terminal vfs file type. Essentially two ring buffers that allow for duplex communication

static void _flush_line_buffer(pseudo_tty* pty) {
    usize len = pty->line_buffer->size;
    u8* data = pty->line_buffer->data;

    ring_buffer_push_array(pty->input_buffer, data, len);
}


static isize _slave_read(vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    return ring_buffer_pop_array(pty->input_buffer, buf, len);
}

static isize _slave_write(vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    ring_buffer_push_array(pty->output_buffer, buf, len);

    if (pty->out_hook)
        pty->out_hook(pty, buf, len);

    return len;
}

static isize _master_read(vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    return ring_buffer_pop_array(pty->output_buffer, buf, len);
}

static void _process_input(vfs_node* node, u8 ch) {
    pseudo_tty* pty = node->private;

    if (pty->flags & PTY_CANONICAL) {
        if (ch == PTY_DELETE_CHAR)
            vec_pop(pty->line_buffer, NULL);

        if (ch == PTY_RETURN_CHAR)
            _flush_line_buffer(pty);
        else
            vec_push(pty->line_buffer, &ch);
    } else {
        ring_buffer_push(pty->input_buffer, ch);
    }

    if (pty->flags & PTY_ECHO)
        _slave_write(node, &ch, 0, 1);
}

static isize _master_write(vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    u8* bytes = (u8*)buf;

    for (usize i = 0; i < len; i++)
        _process_input(node, bytes[i]);

    if (pty->in_hook)
        pty->in_hook(pty, buf, len);

    return len;
}


pseudo_tty* pty_create(usize buffer_size) {
    pseudo_tty* ret = kcalloc(sizeof(pseudo_tty));

    ret->input_buffer = ring_buffer_create(buffer_size);
    ret->output_buffer = ring_buffer_create(buffer_size);

    ret->line_buffer = vec_create(sizeof(char));

    // WARN: this has circular pointers
    ret->master = vfs_create_node(NULL, VFS_CHARDEV);
    ret->master->interface = vfs_create_file_interface(_master_read, _master_write);
    ret->master->private = ret;

    ret->slave = vfs_create_node(NULL, VFS_CHARDEV);
    ret->slave->interface = vfs_create_file_interface(_slave_read, _slave_write);
    ret->slave->private = ret;

    return ret;
}

void pty_destroy(pseudo_tty* pty) {
    ring_buffer_destroy(pty->input_buffer);
    ring_buffer_destroy(pty->output_buffer);

    vec_destroy(pty->line_buffer);

    vfs_destroy_node(pty->master);
    vfs_destroy_node(pty->slave);

    kfree(pty);
}
