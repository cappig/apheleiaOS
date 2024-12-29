#include "mouse.h"

#include <base/types.h>
#include <data/ring.h>
#include <data/vector.h>
#include <input/mouse.h>
#include <log/log.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "arch/panic.h"
#include "mem/heap.h"
#include "sys/video.h"
#include "vfs/fs.h"

static vector* mice = NULL;

// All the mice send their input to a single buffer

static i32 x_pos = 10;
static i32 y_pos = 10;


static ring_buffer* buffer;

static isize _read(UNUSED vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    if (!buf)
        return -1;

    bool popped = ring_buffer_pop_array(buffer, buf, len);

    return popped ? len : 0;
}


void mouse_handle_event(mouse_event event) {
    x_pos = clamp(x_pos + event.delta_x, 0, video.width);
    y_pos = clamp(y_pos + event.delta_y, 0, video.height);


    ring_buffer_push_array(buffer, (u8*)&event, sizeof(mouse_event));

#ifdef INPUT_DEBUG
    log_debug("[INPUT_DEBUG] mouse #%u, x = %i, y = %i", event.source, x_pos, y_pos);
#endif
}

u8 register_mouse(char* name) {
    if (!mice)
        mouse_init();

    mouse* mse = kcalloc(sizeof(mouse));

    mse->name = strdup(name);

    vec_push(mice, mse);

    log_info("Mouse input device registered: %s", name);

    return mice->size - 1;
}

bool mouse_init() {
    vfs_node* dev = vfs_create_node("mouse", VFS_CHARDEV);
    dev->interface = vfs_create_file_interface(_read, NULL);

    vfs_mount("/dev", tree_create_node(dev));

    mice = vec_create(sizeof(mouse));

    buffer = ring_buffer_create(MOUSE_DEV_BUFFER_SIZE);

    x_pos = video.width / 2;
    y_pos = video.height / 2;

    return true;
}
