#include "fsutil.h"

#include <stdio.h>
#include <string.h>

bool fs_is_dir_mode(mode_t mode) {
    return (mode & S_IFMT) == S_IFDIR;
}

const char* fs_path_basename(const char* path) {
    if (!path)
        return "";

    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/')
        len--;

    const char* slash = NULL;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/')
            slash = &path[i];
    }

    return slash ? slash + 1 : path;
}

void fs_join_path(char* out, size_t out_len, const char* left, const char* right) {
    if (!out || !out_len)
        return;

    if (!left || !left[0]) {
        snprintf(out, out_len, "%s", right ? right : "");
        return;
    }

    size_t len = strlen(left);

    if (len > 0 && left[len - 1] == '/')
        snprintf(out, out_len, "%s%s", left, right ? right : "");
    else
        snprintf(out, out_len, "%s/%s", left, right ? right : "");
}

static char fs_mode_type(mode_t mode) {
    switch (mode & S_IFMT) {
    case S_IFDIR:
        return 'd';
    case S_IFLNK:
        return 'l';
    case S_IFCHR:
        return 'c';
    case S_IFBLK:
        return 'b';
    case S_IFIFO:
        return 'p';
    case S_IFSOCK:
        return 's';
    default:
        return '-';
    }
}

void fs_format_mode(mode_t mode, char out[11]) {
    if (!out)
        return;

    out[0] = fs_mode_type(mode);
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    if (mode & S_ISUID)
        out[3] = (mode & S_IXUSR) ? 's' : 'S';

    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    if (mode & S_ISGID)
        out[6] = (mode & S_IXGRP) ? 's' : 'S';

    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';
    if (mode & S_ISVTX)
        out[9] = (mode & S_IXOTH) ? 't' : 'T';

    out[10] = '\0';
}

void fs_format_time_short(time_t t, char* out, size_t out_len) {
    if (!out || !out_len)
        return;

    struct tm tm_val;

    if (!gmtime_r(&t, &tm_val) || !strftime(out, out_len, "%b %e %H:%M", &tm_val))
        snprintf(out, out_len, "??? ?? ??:??");
}
