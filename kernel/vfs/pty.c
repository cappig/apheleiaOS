#include "pty.h"

#include "data/ring.h"
#include "mem/heap.h"
#include "vfs/fs.h"

// A pseudo terminal vfs file type. Essentially two ring buffers tat allow for duplex communication

static isize _master_read(vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    vfs_pty* pty = node->private;
    if (!pty || !buf)
        return -1;

    return ring_buffer_pop_array(pty->output_buffer, buf, len);
}

static isize _master_write(vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    vfs_pty* pty = node->private;
    if (!pty || !buf)
        return -1;

    ring_buffer_push_array(pty->input_buffer, buf, len);

    // TODO: Nope
    // pty->read(pty, buf, len);

    return len;
}

static isize _slave_read(vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    vfs_pty* pty = node->private;
    if (!pty || !buf)
        return -1;

    return ring_buffer_pop_array(pty->input_buffer, buf, len);
}

static isize _slave_write(vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    vfs_pty* pty = node->private;
    if (!pty || !buf)
        return -1;

    ring_buffer_push_array(pty->output_buffer, buf, len);

    pty->write(pty, buf, len);

    return len;
}


vfs_pty* pty_create(usize buffer_size) {
    vfs_pty* ret = kcalloc(sizeof(vfs_pty));

    ret->input_buffer = ring_buffer_create(buffer_size);
    ret->output_buffer = ring_buffer_create(buffer_size);

    // WARN: this has circular pointers
    ret->master = vfs_create_node(NULL, VFS_CHARDEV);
    ret->master->interface = vfs_create_file_interface(_master_read, _master_write);
    ret->master->private = ret;

    ret->slave = vfs_create_node(NULL, VFS_CHARDEV);
    ret->slave->interface = vfs_create_file_interface(_slave_read, _slave_write);
    ret->slave->private = ret;

    return ret;
}

void pty_destroy(vfs_pty* pty) {
    vfs_destroy_node(pty->master);
    vfs_destroy_node(pty->slave);

    kfree(pty);
}
