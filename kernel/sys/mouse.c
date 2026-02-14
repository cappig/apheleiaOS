#include "mouse.h"

#include <base/types.h>
#include <data/ring.h>
#include <data/vector.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/framebuffer.h>

#include "input.h"

static vector_t* mice = NULL;
static ring_buffer_t* buffer = NULL;

static i32 mouse_x = 0;
static i32 mouse_y = 0;

static bool _vec_push_ptr(vector_t* vec, void* ptr) {
    return vec_push(vec, &ptr);
}

static char* _strdup(const char* src) {
    if (!src)
        return NULL;

    size_t len = strlen(src);
    char* out = malloc(len + 1);

    if (!out)
        return NULL;

    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

static i32 _clamp_i32(i32 value, i32 min, i32 max) {
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

ssize_t mouse_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)offset;
    (void)flags;

    if (!buf || !buffer)
        return -1;

    size_t popped = ring_buffer_pop_array(buffer, buf, len);
    return popped ? (ssize_t)popped : 0;
}

void mouse_handle_event(mouse_event event) {
    const framebuffer_info_t* fb = framebuffer_get_info();

    if (fb && fb->available && fb->width && fb->height) {
        mouse_x = _clamp_i32(mouse_x + event.delta_x, 0, (i32)fb->width - 1);
        mouse_y = _clamp_i32(mouse_y + event.delta_y, 0, (i32)fb->height - 1);
    } else {
        mouse_x += event.delta_x;
        mouse_y += event.delta_y;
    }

    if (buffer)
        ring_buffer_push_array(buffer, (u8*)&event, sizeof(event));

    input_push_mouse_event(&event);
}

u8 mouse_register(const char* name) {
    if (!mice || !buffer)
        mouse_init();

    if (!mice || !buffer)
        return 0;

    mouse_dev_t* mse = calloc(1, sizeof(mouse_dev_t));

    if (!mse)
        return 0;

    mse->name = _strdup(name);

    if (!_vec_push_ptr(mice, mse)) {
        free((void*)mse->name);
        free(mse);
        return 0;
    }

    log_info("mouse: registered %s", mse->name ? mse->name : "device");
    return (u8)(mice->size - 1);
}

bool mouse_init(void) {
    if (!mice)
        mice = vec_create(sizeof(mouse_dev_t*));

    if (!mice)
        return false;

    if (!buffer)
        buffer = ring_buffer_create(MOUSE_DEV_BUFFER_SIZE);

    if (!buffer)
        return false;

    const framebuffer_info_t* fb = framebuffer_get_info();
    if (fb && fb->available) {
        mouse_x = (i32)(fb->width / 2);
        mouse_y = (i32)(fb->height / 2);
    }

    return true;
}
