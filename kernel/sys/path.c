#include "path.h"

#include <base/macros.h>
#include <string.h>

static void _pop(char* out, size_t* len, size_t* seg_pos, size_t* seg_count) {
    if (!out || !len || !seg_pos || !seg_count)
        return;

    if (!*seg_count) {
        *len = 1;
        out[1] = '\0';
        return;
    }

    size_t pos = seg_pos[*seg_count - 1];
    if (pos > 1)
        *len = pos - 1;
    else
        *len = 1;

    out[*len] = '\0';
    (*seg_count)--;
}

static bool _push(
    char* out,
    size_t out_len,
    size_t* len,
    const char* seg,
    size_t seg_len,
    size_t* seg_pos,
    size_t* seg_count,
    size_t seg_cap
) {
    if (!out || !len || !seg || !seg_pos || !seg_count)
        return false;

    if (*seg_count >= seg_cap)
        return false;

    size_t extra = (*len > 1) ? 1 : 0;
    if (*len + extra + seg_len >= out_len)
        return false;

    if (*len > 1)
        out[(*len)++] = '/';

    seg_pos[*seg_count] = *len;
    (*seg_count)++;

    memcpy(out + *len, seg, seg_len);
    *len += seg_len;
    out[*len] = '\0';

    return true;
}

static bool _apply(
    const char* input,
    char* out,
    size_t out_len,
    size_t* seg_pos,
    size_t* seg_count,
    size_t seg_cap,
    size_t* len
) {
    if (!input || !out || !seg_pos || !seg_count || !len)
        return false;

    const char* cursor = input;

    while (*cursor) {
        while (*cursor == '/')
            cursor++;

        if (!*cursor)
            break;

        const char* start = cursor;
        while (*cursor && *cursor != '/')
            cursor++;

        size_t seg_len = (size_t)(cursor - start);

        if (seg_len == 1 && start[0] == '.')
            continue;

        if (seg_len == 2 && start[0] == '.' && start[1] == '.') {
            _pop(out, len, seg_pos, seg_count);
            continue;
        }

        if (!_push(out, out_len, len, start, seg_len, seg_pos, seg_count, seg_cap))
            return false;
    }

    return true;
}

bool path_resolve(const char* cwd, const char* path, char* out, size_t out_len) {
    if (!out || out_len < 2 || !path || !path[0])
        return false;

    size_t seg_pos[PATH_MAX / 2] = {0};
    size_t seg_count = 0;
    size_t len = 1;

    out[0] = '/';
    out[1] = '\0';

    if (path[0] != '/') {
        const char* base = (cwd && cwd[0]) ? cwd : "/";

        if (!_apply(base, out, out_len, seg_pos, &seg_count, ARRAY_LEN(seg_pos), &len))
            return false;
    } else {
        seg_count = 0;
        len = 1;
        out[0] = '/';
        out[1] = '\0';
    }

    if (!_apply(path, out, out_len, seg_pos, &seg_count, ARRAY_LEN(seg_pos), &len))
        return false;

    if (!len) {
        out[0] = '/';
        out[1] = '\0';
    }

    return true;
}
