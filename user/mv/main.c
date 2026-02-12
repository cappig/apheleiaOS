#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_str(const char* text) {
    if (!text)
        return;

    write(STDOUT_FILENO, text, strlen(text));
}

static void print_error(const char* src, const char* dst) {
    char line[320];
    snprintf(
        line, sizeof(line), "mv: %s -> %s: %d\n", src ? src : "(null)", dst ? dst : "(null)", errno
    );
    write_str(line);
}

static bool is_dir_mode(mode_t mode) {
    return (mode & S_IFMT) == S_IFDIR;
}

static const char* path_basename(const char* path) {
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

static void join_path(char* out, size_t out_len, const char* left, const char* right) {
    if (!out || out_len == 0)
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

int main(int argc, char** argv) {
    if (argc < 3) {
        write_str("usage: mv SOURCE... DEST\n");
        return 1;
    }

    const char* dest = argv[argc - 1];
    stat_t st_dest = {0};
    bool dest_exists = (stat(dest, &st_dest) == 0);
    bool dest_is_dir = dest_exists && is_dir_mode(st_dest.st_mode);

    if (argc > 3 && !dest_is_dir) {
        write_str("mv: destination is not a directory\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc - 1; i++) {
        const char* src = argv[i];
        char target[PATH_MAX];

        if (dest_is_dir)
            join_path(target, sizeof(target), dest, path_basename(src));
        else
            snprintf(target, sizeof(target), "%s", dest);

        if (rename(src, target) < 0) {
            print_error(src, target);
            rc = 1;
        }
    }

    return rc;
}
