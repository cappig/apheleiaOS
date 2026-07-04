#include "path.h"

#include <base/macros.h>
#include <string.h>

typedef struct {
    char *out;
    size_t out_len;
    size_t len;
    size_t *seg_pos;
    size_t seg_count;
    size_t seg_cap;
} path_build_t;

static void _pop(path_build_t *path) {
    if (!path || !path->out || !path->seg_pos) {
        return;
    }

    if (!path->seg_count) {
        path->len = 1;
        path->out[1] = '\0';
        return;
    }

    size_t pos = path->seg_pos[path->seg_count - 1];

    if (pos > 1) {
        path->len = pos - 1;
    } else {
        path->len = 1;
    }

    path->out[path->len] = '\0';
    path->seg_count--;
}

static bool _push(path_build_t *path, const char *seg, size_t seg_len) {
    if (!path || !path->out || !seg || !path->seg_pos) {
        return false;
    }

    if (path->seg_count >= path->seg_cap) {
        return false;
    }

    size_t extra = (path->len > 1) ? 1 : 0;
    if (path->len + extra + seg_len >= path->out_len) {
        return false;
    }

    if (path->len > 1) {
        path->out[path->len++] = '/';
    }

    path->seg_pos[path->seg_count] = path->len;
    path->seg_count++;

    memcpy(path->out + path->len, seg, seg_len);
    path->len += seg_len;
    path->out[path->len] = '\0';

    return true;
}

static bool _apply(path_build_t *path, const char *input) {
    if (!path || !input) {
        return false;
    }

    const char *cursor = input;

    while (*cursor) {
        while (*cursor == '/') {
            cursor++;
        }

        if (!*cursor) {
            break;
        }

        const char *start = cursor;
        while (*cursor && *cursor != '/') {
            cursor++;
        }

        size_t seg_len = (size_t)(cursor - start);

        if (seg_len == 1 && start[0] == '.') {
            continue;
        }

        if (seg_len == 2 && start[0] == '.' && start[1] == '.') {
            _pop(path);
            continue;
        }

        bool pushed = _push(path, start, seg_len);

        if (!pushed) {
            return false;
        }
    }

    return true;
}

bool path_resolve(const char *cwd, const char *path, char *out, size_t out_len) {
    if (!out || out_len < 2 || !path || !path[0]) {
        return false;
    }

    size_t seg_pos[PATH_MAX / 2] = { 0 };
    path_build_t built = {
        .out = out,
        .out_len = out_len,
        .len = 1,
        .seg_pos = seg_pos,
        .seg_cap = ARRAY_LEN(seg_pos),
    };

    out[0] = '/';
    out[1] = '\0';

    if (path[0] != '/') {
        const char *base = (cwd && cwd[0]) ? cwd : "/";

        bool applied_base = _apply(&built, base);

        if (!applied_base) {
            return false;
        }
    } else {
        built.seg_count = 0;
        built.len = 1;
        built.out[0] = '/';
        built.out[1] = '\0';
    }

    bool applied_path = _apply(&built, path);

    if (!applied_path) {
        return false;
    }

    if (!built.len) {
        built.out[0] = '/';
        built.out[1] = '\0';
    }

    return true;
}
