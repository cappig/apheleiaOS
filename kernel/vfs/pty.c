#include "pty.h"

#include <base/types.h>
#include <data/ring.h>
#include <data/vector.h>
#include <string.h>

#include "log/log.h"
#include "mem/heap.h"
#include "sys/cpu.h"
#include "vfs/fs.h"

// A pseudo terminal vfs file type. Essentially two ring buffers that allow for duplex communication

static void flush_line_buffer(pseudo_tty* pty) {
    usize len = pty->line_buffer->size;
    u8* data = pty->line_buffer->data;

    ring_buffer_push_array(pty->input_buffer, data, len);

    vec_clear(pty->line_buffer);
}


static isize slave_read(vfs_node* node, void* buf, UNUSED usize offset, usize len, u32 flags) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    bool no_block = flags & VFS_NONBLOCK;

    if (ring_buffer_is_empty(pty->input_buffer) && cpu->sched_running && !no_block)
        wait_list_append(pty->waiters, cpu->sched->current);

    return ring_buffer_pop_array(pty->input_buffer, buf, len);
}

static isize slave_write(vfs_node* node, void* buf, UNUSED usize offset, usize len, u32 flags) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    ring_buffer_push_array(pty->output_buffer, buf, len);

    if (pty->out_hook)
        pty->out_hook(pty, buf, len);

    return len;
}

static bool process_canonical_input(vfs_node* node, u8* buf, usize len, usize flags) {
    pseudo_tty* pty = node->private;

    bool has_data = false;

    for (usize i = 0; i < len; i++) {
        char ch = buf[i];

        if (ch == PTY_RETURN_CHAR) {
            flush_line_buffer(pty);

            char nl[] = "\n";
            slave_write(node, nl, 0, 1, flags);

            has_data = true;

            continue;
        }

        if (ch == PTY_DELETE_CHAR) {
            if (!pty->line_buffer->size)
                continue;

            vec_pop(pty->line_buffer, NULL);

            if (pty->flags & PTY_ECHO) {
                char space[] = "\b \b"; // wtf?
                slave_write(node, space, 0, 3, flags);
            }

            continue;
        }

        if (pty->flags & PTY_ECHO)
            slave_write(node, &ch, 0, 1, flags);

        vec_push(pty->line_buffer, &ch);
    }

    return has_data;
}

static isize master_read(vfs_node* node, void* buf, UNUSED usize offset, usize len, u32 flags) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    return ring_buffer_pop_array(pty->output_buffer, buf, len);
}

static isize master_write(vfs_node* node, void* buf, UNUSED usize offset, usize len, u32 flags) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    bool has_data = false;

    if (pty->flags & PTY_CANONICAL) {
        has_data = process_canonical_input(node, buf, len, flags);
    } else {
        ring_buffer_push_array(pty->input_buffer, buf, len);

        if (pty->flags & PTY_ECHO)
            slave_write(node, buf, offset, len, flags);

        has_data = true;
    }

    if (has_data) {
        if (pty->in_hook)
            pty->in_hook(pty, buf, len);

        wait_list_wake_up(pty->waiters);
    }

    return len;
}


pseudo_tty* pty_create(usize buffer_size) {
    pseudo_tty* ret = kcalloc(sizeof(pseudo_tty));

    ret->input_buffer = ring_buffer_create(buffer_size);
    ret->output_buffer = ring_buffer_create(buffer_size);

    ret->line_buffer = vec_create_sized(10, sizeof(char));

    // WARN: this has circular pointers
    ret->master = vfs_create_node(NULL, VFS_CHARDEV);
    ret->master->interface = vfs_create_interface(master_read, master_write);
    ret->master->private = ret;

    ret->slave = vfs_create_node(NULL, VFS_CHARDEV);
    ret->slave->interface = vfs_create_interface(slave_read, slave_write);
    ret->slave->private = ret;

    ret->waiters = wait_list_create();

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
