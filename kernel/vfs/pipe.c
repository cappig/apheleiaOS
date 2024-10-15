#include "pipe.h"

#include <base/attributes.h>
#include <base/types.h>
#include <data/ring.h>

#include "fs.h"
#include "mem/heap.h"


static isize pipe_read(vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    ring_buffer* ring = node->private;

    if (!ring || !buf)
        return -1;

    return ring_buffer_pop_array(ring, buf, len);
}

static isize pipe_write(vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    ring_buffer* ring = node->private;

    if (!ring || !buf)
        return -1;

    ring_buffer_push_array(ring, buf, len);

    return len;
}


vfs_node* pipe_create(char* name, usize size) {
    ring_buffer* ring = ring_buffer_create(size);

    vfs_node_interface* interface = kcalloc(sizeof(vfs_node_interface));
    interface->read = pipe_read;
    interface->write = pipe_write;

    vfs_node* vnode = vfs_create_node(name, VFS_CHARDEV);
    vnode->private = ring;
    vnode->interface = interface;

    return vnode;
}

void pipe_destroy(vfs_node* pipe) {
    if (pipe->type != VFS_CHARDEV)
        return;

    ring_buffer_destroy((ring_buffer*)pipe->private);
    kfree(pipe->interface);
    kfree(pipe);
}
