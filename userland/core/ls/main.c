#include <account.h>
#include <dirent.h>
#include <errno.h>
#include <fsutil.h>
#include <io.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define LS_C_RESET "\x1b[0m"
#define LS_C_GREEN "\x1b[32m"
#define LS_C_BLUE  "\x1b[34m"

typedef struct {
    bool all;
    bool almost_all;
    bool long_format;
    bool single_column;
    bool human_size;
    bool recursive;
    bool color;
} ls_opts_t;

typedef struct {
    char name[NAME_MAX + 1];
    unsigned char type;
    struct stat st;
    bool have_stat;
} ls_entry_t;

typedef struct {
    ls_entry_t *items;
    size_t count;
    size_t cap;
} ls_entries_t;

typedef struct {
    size_t name;
    size_t links;
    size_t user;
    size_t group;
    size_t size;
} ls_widths_t;

static size_t decimal_width_u64(unsigned long long value) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%llu", value);
    return (len > 0) ? (size_t)len : 1;
}

static void format_size(unsigned long long size, bool human, char *out, size_t out_len) {
    if (!out || !out_len) {
        return;
    }

    if (!human) {
        snprintf(out, out_len, "%llu", size);
        return;
    }

    static const char suffixes[] = "BKMGT";
    unsigned long long whole = size;
    unsigned long long frac = 0;
    size_t suffix = 0;

    while (whole >= 1024 && suffix + 1 < sizeof(suffixes) - 1) {
        frac = ((whole % 1024) * 10 + 512) / 1024;
        whole /= 1024;
        suffix++;

        if (frac >= 10) {
            whole++;
            frac = 0;
        }
    }

    if (!suffix) {
        snprintf(out, out_len, "%llu", whole);
    } else if (whole < 10 && frac) {
        snprintf(out, out_len, "%llu.%llu%c", whole, frac, suffixes[suffix]);
    } else {
        snprintf(out, out_len, "%llu%c", whole, suffixes[suffix]);
    }
}

static size_t size_width(unsigned long long size, bool human) {
    char buf[32];
    format_size(size, human, buf, sizeof(buf));
    return strlen(buf);
}

static bool is_dir_mode(mode_t mode) {
    return (mode & S_IFMT) == S_IFDIR;
}

static bool is_exec_mode(mode_t mode) {
    return (mode & S_IFMT) == S_IFREG && (mode & (S_IXUSR | S_IXGRP | S_IXOTH));
}

static bool entry_is_dir(const ls_entry_t *entry) {
    if (!entry) {
        return false;
    }

    if (entry->type == DT_DIR) {
        return true;
    }

    return entry->have_stat && is_dir_mode(entry->st.st_mode);
}

static bool entry_can_be_exec(const ls_entry_t *entry) {
    if (!entry) {
        return false;
    }

    return entry->type == DT_REG || entry->type == DT_UNKNOWN;
}

static const char *entry_color(const ls_entry_t *entry) {
    if (!entry) {
        return NULL;
    }

    if (entry_is_dir(entry)) {
        return LS_C_BLUE;
    }

    if (entry->have_stat && is_exec_mode(entry->st.st_mode)) {
        return LS_C_GREEN;
    }

    return NULL;
}

static void write_entry_name(const char *name, const ls_entry_t *entry, bool color) {
    const char *prefix = color ? entry_color(entry) : NULL;

    if (prefix) {
        io_write_str(prefix);
    }

    io_write_str(name ? name : "");

    if (prefix) {
        io_write_str(LS_C_RESET);
    }
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

static bool read_entry_stat(const char *path, ls_entry_t *entry) {
    if (!path || !entry) {
        return false;
    }

    if (entry->have_stat) {
        return true;
    }

    char full[256];
    if (!fs_join_path(full, sizeof(full), path, entry->name)) {
        return false;
    }

    if (stat(full, &entry->st) < 0) {
        memset(&entry->st, 0, sizeof(entry->st));
        return false;
    }

    entry->have_stat = true;
    return true;
}

static bool add_entry(ls_entries_t *entries, const struct dirent *ent) {
    if (!entries || !ent) {
        return false;
    }

    if (entries->count >= entries->cap) {
        size_t new_cap = entries->cap ? entries->cap * 2 : 32;

        if (new_cap < entries->cap || new_cap > ((size_t)-1) / sizeof(*entries->items)) {
            return false;
        }

        ls_entry_t *new_items = realloc(entries->items, new_cap * sizeof(*new_items));

        if (!new_items) {
            return false;
        }

        entries->items = new_items;
        entries->cap = new_cap;
    }

    ls_entry_t *entry = &entries->items[entries->count++];

    memset(entry, 0, sizeof(*entry));
    snprintf(entry->name, sizeof(entry->name), "%s", ent->d_name);
    entry->type = ent->d_type;

    return true;
}

static size_t term_width(void) {
    winsize_t ws;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_col) {
        return ws.ws_col;
    }

    return 80;
}

static int print_file(const char *path, const char *name, const ls_opts_t *opts) {
    if (!path || !opts) {
        return 1;
    }

    struct stat st = { 0 };
    if (stat(path, &st) < 0) {
        char msg[320];
        snprintf(msg, sizeof(msg), "ls: %s: %s\n", path, strerror(errno));
        io_write_str(msg);
        return 1;
    }

    if (opts->long_format) {
        char uid_buf[16];
        char gid_buf[16];
        const char *uname = account_uid_name(st.st_uid, uid_buf, sizeof(uid_buf));
        const char *gname = account_gid_name(st.st_gid, gid_buf, sizeof(gid_buf));

        char mode[11];
        fs_format_mode(st.st_mode, mode);

        char timebuf[32];
        fs_format_time_short(st.st_mtime, timebuf, sizeof(timebuf));

        char sizebuf[32];
        format_size((unsigned long long)st.st_size, opts->human_size, sizebuf, sizeof(sizebuf));

        char line[256];
        snprintf(
            line,
            sizeof(line),
            "%s %lu %s %s %s %s ",
            mode,
            (unsigned long)st.st_nlink,
            uname,
            gname,
            sizebuf,
            timebuf
        );

        io_write_str(line);
        ls_entry_t entry = {
            .type = fs_is_dir_mode(st.st_mode) ? DT_DIR : DT_REG,
            .st = st,
            .have_stat = true,
        };

        write_entry_name(name ? name : path, &entry, opts->color);
        io_write_char('\n');
        return 0;
    }

    ls_entry_t entry = {
        .type = fs_is_dir_mode(st.st_mode) ? DT_DIR : DT_REG,
        .st = st,
        .have_stat = true,
    };

    write_entry_name(name ? name : path, &entry, opts->color);
    io_write_char('\n');

    return 0;
}

static bool read_entries(DIR *dir, const ls_opts_t *opts, ls_entries_t *entries) {
    struct dirent *entry = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (!want_name(entry->d_name, opts->all, opts->almost_all)) {
            continue;
        }

        if (!add_entry(entries, entry)) {
            return false;
        }
    }

    return true;
}

static void measure_entries(const char *path, const ls_opts_t *opts, ls_entries_t *entries, ls_widths_t *widths) {
    for (size_t i = 0; i < entries->count; i++) {
        ls_entry_t *entry = &entries->items[i];

        size_t name_len = strlen(entry->name);
        if (name_len > widths->name) {
            widths->name = name_len;
        }

        if (opts->color && !opts->long_format && entry_can_be_exec(entry)) {
            read_entry_stat(path, entry);
        }

        if (!opts->long_format) {
            continue;
        }

        char uid_buf[16];
        char gid_buf[16];
        read_entry_stat(path, entry);

        const char *user = account_uid_name(entry->st.st_uid, uid_buf, sizeof(uid_buf));
        const char *group = account_gid_name(entry->st.st_gid, gid_buf, sizeof(gid_buf));

        widths->user = max(widths->user, strlen(user));
        widths->group = max(widths->group, strlen(group));
        widths->links = max(widths->links, decimal_width_u64((unsigned long long)entry->st.st_nlink));
        widths->size = max(widths->size, size_width((unsigned long long)entry->st.st_size, opts->human_size));
    }
}

static void print_long_entry(const ls_entry_t *entry, const ls_opts_t *opts, const ls_widths_t *widths) {
    char uid_buf[16];
    char gid_buf[16];
    const char *user = account_uid_name(entry->st.st_uid, uid_buf, sizeof(uid_buf));
    const char *group = account_gid_name(entry->st.st_gid, gid_buf, sizeof(gid_buf));

    char mode[11];
    fs_format_mode(entry->st.st_mode, mode);

    char time[32];
    fs_format_time_short(entry->st.st_mtime, time, sizeof(time));

    char size[32];
    format_size((unsigned long long)entry->st.st_size, opts->human_size, size, sizeof(size));

    char line[256];
    snprintf(
        line,
        sizeof(line),
        "%s %*lu %-*s %-*s %*s %s ",
        mode,
        (int)widths->links,
        (unsigned long)entry->st.st_nlink,
        (int)widths->user,
        user,
        (int)widths->group,
        group,
        (int)widths->size,
        size,
        time
    );

    io_write_str(line);
    write_entry_name(entry->name, entry, opts->color);
    io_write_char('\n');
}

static void print_entries(const ls_entries_t *entries, const ls_opts_t *opts, const ls_widths_t *widths) {
    size_t column_width = widths->name + 2;
    size_t columns = 1;

    if (!opts->long_format && !opts->single_column && column_width) {
        columns = max((size_t)1, term_width() / column_width);
    }

    size_t column = 0;

    for (size_t i = 0; i < entries->count; i++) {
        const ls_entry_t *entry = &entries->items[i];

        if (opts->long_format) {
            print_long_entry(entry, opts, widths);
            continue;
        }

        write_entry_name(entry->name, entry, opts->color);

        if (opts->single_column || columns == 1) {
            io_write_char('\n');
            column = 0;
            continue;
        }

        size_t name_len = strlen(entry->name);
        size_t padding = column_width > name_len ? column_width - name_len : 1;
        io_write_repeat(' ', padding);

        if (++column >= columns) {
            io_write_char('\n');
            column = 0;
        }
    }

    if (!opts->long_format && !opts->single_column && column) {
        io_write_char('\n');
    }
}

static int list_dir(const char *path, const ls_opts_t *opts, bool print_header) {
    if (!path || !opts) {
        return 1;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        char msg[320];
        snprintf(msg, sizeof(msg), "ls: %s: %s\n", path, strerror(errno));
        io_write_str(msg);
        return 1;
    }

    if (print_header) {
        io_write_str(path);
        io_write_str(":\n");
    }

    ls_entries_t entries = { 0 };
    bool read_ok = read_entries(dir, opts, &entries);

    closedir(dir);

    if (!read_ok) {
        free(entries.items);
        io_write_str("ls: out of memory\n");
        return 1;
    }

    ls_widths_t widths = {
        .links = 1,
        .user = 1,
        .group = 1,
        .size = 1,
    };

    measure_entries(path, opts, &entries, &widths);
    print_entries(&entries, opts, &widths);

    int status = 0;

    if (opts->recursive) {
        for (size_t i = 0; i < entries.count; i++) {
            ls_entry_t *entry = &entries.items[i];
            const char *name = entry->name;

            if (!strcmp(name, ".") || !strcmp(name, "..")) {
                continue;
            }

            if (entry->type == DT_UNKNOWN && !read_entry_stat(path, entry)) {
                continue;
            }

            if (!entry_is_dir(entry)) {
                continue;
            }

            char child[256];
            if (!fs_join_path(child, sizeof(child), path, name)) {
                char msg[320];
                snprintf(msg, sizeof(msg), "ls: %s/%s: %s\n", path, name, strerror(errno));
                io_write_str(msg);
                status = 1;
                continue;
            }

            io_write_char('\n');
            if (list_dir(child, opts, true) != 0) {
                status = 1;
            }
        }
    }

    free(entries.items);
    return status;
}

int main(int argc, char **argv) {
    ls_opts_t opts = {
        .color = io_color_enabled(STDOUT_FILENO),
    };

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
                opts.all = true;
                opts.almost_all = false;
                break;
            case 'A':
                opts.almost_all = true;
                break;
            case 'h':
                opts.human_size = true;
                break;
            case 'l':
                opts.long_format = true;
                break;
            case '1':
                opts.single_column = true;
                break;
            case 'R':
                opts.recursive = true;
                break;
            default:
                io_write_str("ls: unknown option\n");
                return 1;
            }
        }
    }

    if (opts.long_format) {
        opts.single_column = true;
    }

    int paths = argc - argi;
    if (paths <= 0) {
        return list_dir(".", &opts, opts.recursive);
    }

    int status = 0;
    for (int i = argi; i < argc; i++) {
        const char *path = argv[i];

        struct stat st;
        if (!path || stat(path, &st) < 0) {
            char msg[320];
            snprintf(msg, sizeof(msg), "ls: %s: %s\n", path ? path : "", strerror(errno));
            io_write_str(msg);
            status = 1;
        } else if (!fs_is_dir_mode(st.st_mode)) {
            status |= print_file(path, path, &opts);
        } else if (list_dir(path, &opts, paths > 1 || opts.recursive) != 0) {
            status = 1;
        }

        if (paths > 1 && i + 1 < argc) {
            io_write_char('\n');
        }
    }

    return status;
}
