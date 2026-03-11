#include "framebuffer.h"

#include <string.h>

static framebuffer_info_t framebuffer_info = {0};

void framebuffer_set_info(const framebuffer_info_t *info) {
    if (!info) {
        memset(&framebuffer_info, 0, sizeof(framebuffer_info));
        return;
    }

    framebuffer_info = *info;
}

const framebuffer_info_t *framebuffer_get_info(void) {
    if (!framebuffer_info.available) {
        return NULL;
    }

    return &framebuffer_info;
}
