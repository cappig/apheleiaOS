#include <account.h>
#include <dirent.h>
#include <fsutil.h>
#include <io.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static size_t decimal_width_u64(unsigned long long value) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%llu", value);
    return (len > 0) ? (size_t)len : 1;
}

static void read_entry_meta(
    const char *dir_path,
    const char *name,
    struct stat *st,
    const char **uname,
    const char **gname,
    char uid_buf[16],
    char gid_buf[16]
) {
    if (!dir_path || !name || !st || !uname || !gname || !uid_buf || !gid_buf) {
        return;
    }

    char full[256];
    fs_join_path(full, sizeof(full), dir_path, name);

    if (stat(full, st) < 0) {
        memset(st, 0, sizeof(*st));
    }

    *uname = account_uid_name(st->st_uid, uid_buf, 16);
    *gname = account_gid_name(st->st_gid, gid_buf, 16);
}

static bool want_name(const char *name, bool opt_all, bool opt_almost) {
    if (!name || !name[0]) {
        return false;
    }

    if (opt_all) {
        return true;
    }

    if (name[0] != '.') {
        return true;
    }

    if (opt_almost) {
        return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
    }

    return false;
}

static size_t term_width(void) {
    winsize_t ws;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_col) {
        return ws.ws_col;
    }

    return 80;
}

static int list_dir(
    const char *path,
    bool opt_all,
    bool opt_almost,
    bool opt_long,
    bool opt_single
) {
    DIR *dir = opendir(path);
    if (!dir) {
        io_write_str("ls: failed to open\n");
        return 1;
    }

    size_t max_len = 0;
    size_t width_links = 1;
    size_t width_uname = 1;
    size_t width_gname = 1;
    size_t width_size = 1;

    struct dirent *ent = NULL;

    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;

        if (!want_name(name, opt_all, opt_almost)) {
            continue;
        }

        size_t len = strlen(name);

        if (len > max_len) {
            max_len = len;
        }

        if (opt_long) {
            struct stat st = {0};
            char uid_buf[16];
            char gid_buf[16];
            const char *uname = "";
            const char *gname = "";

            read_entry_meta(
                path,
                name,
                &st,
                &uname,
                &gname,
                uid_buf,
                gid_buf
            );

            size_t uname_len = strlen(uname);
            if (uname_len > width_uname) {
                width_uname = uname_len;
            }

            size_t gname_len = strlen(gname);
            if (gname_len > width_gname) {
                width_gname = gname_len;
            }

            size_t links_len = decimal_width_u64((unsigned long long)st.st_nlink);
            if (links_len > width_links) {
                width_links = links_len;
            }

            size_t size_len = decimal_width_u64((unsigned long long)st.st_size);
            if (size_len > width_size) {
                width_size = size_len;
            }
        }
    }

    rewinddir(dir);

    size_t cols = 1;
    size_t col_width = max_len + 2;

    if (!opt_long && !opt_single && col_width) {
        cols = term_width() / col_width;

        if (!cols) {
            cols = 1;
        }
    }

    size_t col = 0;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;

        if (!want_name(name, opt_all, opt_almost)) {
            continue;
        }

        if (opt_long) {
            struct stat st = {0};
            char uid_buf[16];
            char gid_buf[16];
            const char *uname = "";
            const char *gname = "";

            read_entry_meta(
                path,
                name,
                &st,
                &uname,
                &gname,
                uid_buf,
                gid_buf
            );

            char mode[11];
            fs_format_mode(st.st_mode, mode);

            char timebuf[32];
            fs_format_time_short(st.st_mtime, timebuf, sizeof(timebuf));

            char line[256];
            snprintf(
                line,
                sizeof(line),
                "%s %*lu %-*s %-*s %*llu %s %s\n",
                mode,
                (int)width_links,
                (unsigned long)st.st_nlink,
                (int)width_uname,
                uname,
                (int)width_gname,
                gname,
                (int)width_size,
                (unsigned long long)st.st_size,
                timebuf,
                name
            );

            io_write_str(line);
            continue;
        }

        io_write_str(name);

        size_t name_len = strlen(name);

        if (opt_single || cols == 1) {
            io_write_char('\n');
            col = 0;
            continue;
        }

        size_t pad = col_width > name_len ? col_width - name_len : 1;
        for (size_t i = 0; i < pad; i++) {
            io_write_char(' ');
        }

        col++;

        if (col >= cols) {
            io_write_char('\n');
            col = 0;
        }
    }

    if (!opt_long && !opt_single && col != 0) {
        io_write_char('\n');
    }

    closedir(dir);
    return 0;
}

int main(int argc, char **argv) {
    bool opt_all = false;
    bool opt_almost = false;
    bool opt_long = false;
    bool opt_single = false;

    int argi = 1;
    for (; argi < argc; argi++) {
        const char *arg = argv[argi];

        if (!arg || arg[0] != '-' || !arg[1]) {
            break;
        }

        if (!strcmp(arg, "--")) {
            argi++;
            break;
        }

        for (size_t i = 1; arg[i]; i++) {
            switch (arg[i]) {
            case 'a':
                opt_all = true;
                opt_almost = false;
                break;
            case 'A':
                opt_almost = true;
                break;
            case 'l':
                opt_long = true;
                break;
            case '1':
                opt_single = true;
                break;
            default:
                io_write_str("ls: unknown option\n");
                return 1;
            }
        }
    }

    if (opt_long) {
        opt_single = true;
    }

    int paths = argc - argi;
    if (paths <= 0) {
        return list_dir(".", opt_all, opt_almost, opt_long, opt_single);
    }

    int status = 0;
    for (int i = argi; i < argc; i++) {
        const char *path = argv[i];

        if (paths > 1) {
            io_write_str(path);
            io_write_str(":\n");
        }

        struct stat st;
        if (!path || stat(path, &st) < 0) {
            if (list_dir(path, opt_all, opt_almost, opt_long, opt_single) != 0) {
                status = 1;
            }
        } else if (!fs_is_dir_mode(st.st_mode)) {
            char msg[320];
            snprintf(msg, sizeof(msg), "ls: %s: not a directory\n", path);
            io_write_str(msg);
        } else if (list_dir(path, opt_all, opt_almost, opt_long, opt_single) != 0) {
            status = 1;
        }

        if (paths > 1 && i + 1 < argc) {
            io_write_char('\n');
        }
    }

    return status;
}
