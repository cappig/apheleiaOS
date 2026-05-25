#include "logsink.h"

#include <string.h>
#include <sys/vfs.h>

#define LOGSINK_TARGET_MAX 16

typedef struct {
    char path[128];
    vfs_node_t *node;
} logsink_target_t;

typedef struct {
    logsink_target_t targets[LOGSINK_TARGET_MAX];
    size_t count;
    bool bound;
} logsink_state_t;

static logsink_state_t logsink = { 0 };

static bool _path_exists(const char *path) {
    if (!path || !path[0]) {
        return false;
    }

    for (size_t i = 0; i < logsink.count; i++) {
        if (!strcmp(logsink.targets[i].path, path)) {
            return true;
        }
    }

    return false;
}

void logsink_reset(void) {
    memset(&logsink, 0, sizeof(logsink));
}

void logsink_add_target(const char *path) {
    if (!path || !path[0] || logsink.count >= LOGSINK_TARGET_MAX || _path_exists(path)) {
        return;
    }

    logsink_target_t *target = &logsink.targets[logsink.count];
    size_t path_len = sizeof(target->path) - 1;

    strncpy(target->path, path, path_len);
    target->path[path_len] = '\0';
    target->node = NULL;
    logsink.count++;
}

void logsink_bind_devices(void) {
    for (size_t i = 0; i < logsink.count; i++) {
        logsink.targets[i].node = vfs_lookup(logsink.targets[i].path);
    }

    logsink.bound = true;
}

void logsink_unbind_devices(void) {
    for (size_t i = 0; i < logsink.count; i++) {
        logsink.targets[i].node = NULL;
    }

    logsink.bound = false;
}

bool logsink_is_bound(void) {
    return logsink.bound;
}

bool logsink_has_targets(void) {
    return logsink.count > 0;
}

void logsink_write(const char *s, size_t len) {
    if (!logsink.bound || !s || !len) {
        return;
    }

    for (size_t i = 0; i < logsink.count; i++) {
        const logsink_target_t *target = &logsink.targets[i];

        if (!target->node) {
            continue;
        }

        vfs_write(target->node, (void *)s, 0, len, VFS_NONBLOCK);
    }
}
