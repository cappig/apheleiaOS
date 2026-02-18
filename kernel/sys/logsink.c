#include "logsink.h"

#include <string.h>
#include <sys/vfs.h>

#define LOGSINK_TARGET_MAX 16

typedef struct {
    char path[128];
    vfs_node_t *node;
} logsink_target_t;

static logsink_target_t logsink_targets[LOGSINK_TARGET_MAX];
static size_t logsink_target_count = 0;
static bool logsink_bound = false;


static bool _path_exists(const char *path) {
    if (!path || !path[0]) {
        return false;
    }

    for (size_t i = 0; i < logsink_target_count; i++) {
        if (!strcmp(logsink_targets[i].path, path)) {
            return true;
        }
    }

    return false;
}

void logsink_reset(void) {
    memset(logsink_targets, 0, sizeof(logsink_targets));
    logsink_target_count = 0;
    logsink_bound = false;
}

void logsink_add_target(const char *path) {
    if (!path || !path[0] || logsink_target_count >= LOGSINK_TARGET_MAX || _path_exists(path)) {
        return;
    }

    size_t path_len = sizeof(logsink_targets[logsink_target_count].path) - 1;
    strncpy(logsink_targets[logsink_target_count].path, path, path_len);

    logsink_targets[logsink_target_count].path[path_len] = '\0';
    logsink_targets[logsink_target_count].node = NULL;
    logsink_target_count++;
}

void logsink_bind_devices(void) {
    for (size_t i = 0; i < logsink_target_count; i++) {
        logsink_targets[i].node = vfs_lookup(logsink_targets[i].path);
    }

    logsink_bound = true;
}

void logsink_unbind_devices(void) {
    for (size_t i = 0; i < logsink_target_count; i++) {
        logsink_targets[i].node = NULL;
    }

    logsink_bound = false;
}

bool logsink_is_bound(void) {
    return logsink_bound;
}

bool logsink_has_targets(void) {
    return logsink_target_count > 0;
}

void logsink_write(const char *s, size_t len) {
    if (!logsink_bound || !s || !len) {
        return;
    }

    for (size_t i = 0; i < logsink_target_count; i++) {
        const logsink_target_t *target = &logsink_targets[i];

        if (!target->node) {
            continue;
        }

        vfs_write(target->node, (void *)s, 0, len, VFS_NONBLOCK);
    }
}
