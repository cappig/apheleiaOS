#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static ssize_t write_str(const char* str) {
    if (!str)
        return 0;

    return write(STDOUT_FILENO, str, strlen(str));
}

static void write_char(char ch) {
    write(STDOUT_FILENO, &ch, 1);
}

static bool want_name(const char* name, bool opt_all, bool opt_almost) {
    if (!name || !name[0])
        return false;

    if (opt_all)
        return true;

    if (name[0] != '.')
        return true;

    if (opt_almost)
        return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;

    return false;
}

static size_t term_width(void) {
    winsize_t ws;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_col)
        return ws.ws_col;

    return 80;
}

static char mode_type(mode_t mode) {
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

static void format_mode(mode_t mode, char out[11]) {
    out[0] = mode_type(mode);
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

static bool is_leap(int year) {
    return ((!(year % 4) && year % 100) || !(year % 400));
}

static int days_in_month(int month, int year) {
    switch (month) {
    case 1:
        return is_leap(year) ? 29 : 28;
    case 3:
    case 5:
    case 8:
    case 10:
        return 30;
    default:
        return 31;
    }
}

static void format_time(time_t t, char* out, size_t out_len) {
    static const char* months[] = {
        "Jan",
        "Feb",
        "Mar",
        "Apr",
        "May",
        "Jun",
        "Jul",
        "Aug",
        "Sep",
        "Oct",
        "Nov",
        "Dec",
    };

    if (!out || !out_len)
        return;

    if (t < 0)
        t = 0;

    unsigned long long seconds = (unsigned long long)t;
    unsigned long long days = seconds / 86400;
    unsigned long long rem = seconds % 86400;
    int hour = (int)(rem / 3600);
    int min = (int)((rem % 3600) / 60);

    int year = 1970;
    for (;;) {
        int year_days = is_leap(year) ? 366 : 365;

        if (days < (unsigned long long)year_days)
            break;

        days -= (unsigned long long)year_days;
        year++;
    }

    int month = 0;
    while (month < 12) {
        int dim = days_in_month(month, year);

        if (days < (unsigned long long)dim)
            break;

        days -= (unsigned long long)dim;
        month++;
    }

    int mday = (int)days + 1;

    snprintf(out, out_len, "%s %2d %02d:%02d", months[month], mday, hour, min);
}

static const char* uid_name(uid_t uid, char* buf, size_t len) {
    if (!buf || !len)
        return "";

    passwd_t pwd = {0};
    if (!getpwuid(uid, &pwd) && pwd.pw_name[0]) {
        snprintf(buf, len, "%s", pwd.pw_name);
        return buf;
    }

    snprintf(buf, len, "%llu", (unsigned long long)uid);
    return buf;
}

static const char* gid_name(gid_t gid, char* buf, size_t len) {
    if (!buf || !len)
        return "";

    group_t grp = {0};
    if (!getgrgid(gid, &grp) && grp.gr_name[0]) {
        snprintf(buf, len, "%s", grp.gr_name);
        return buf;
    }

    snprintf(buf, len, "%llu", (unsigned long long)gid);
    return buf;
}

static void join_path(char* out, size_t out_len, const char* dir, const char* name) {
    if (!out || !out_len) {
        return;
    }

    if (!dir || !dir[0]) {
        snprintf(out, out_len, "%s", name ? name : "");
        return;
    }

    size_t dlen = strlen(dir);

    if (dlen && dir[dlen - 1] == '/')
        snprintf(out, out_len, "%s%s", dir, name ? name : "");
    else
        snprintf(out, out_len, "%s/%s", dir, name ? name : "");
}

static int
list_dir(const char* path, bool opt_all, bool opt_almost, bool opt_long, bool opt_single) {
    int fd = open(path, O_RDONLY, 0);

    if (fd < 0) {
        write_str("ls: failed to open\n");
        return 1;
    }

    size_t max_len = 0;
    size_t width_links = 1;
    size_t width_uname = 1;
    size_t width_gname = 1;
    size_t width_size = 1;
    dirent_t ent;

    while (getdents(fd, &ent) > 0) {
        char name[DIRENT_NAME_MAX];
        strncpy(name, ent.d_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';

        if (!want_name(name, opt_all, opt_almost))
            continue;

        size_t len = strlen(name);

        if (len > max_len)
            max_len = len;

        if (opt_long) {
            char full[256];
            stat_t st;

            join_path(full, sizeof(full), path, name);

            if (stat(full, &st) < 0)
                memset(&st, 0, sizeof(st));

            char uid_buf[16];
            char gid_buf[16];
            const char* uname = uid_name(st.st_uid, uid_buf, sizeof(uid_buf));
            const char* gname = gid_name(st.st_gid, gid_buf, sizeof(gid_buf));

            size_t uname_len = strlen(uname);
            size_t gname_len = strlen(gname);
            if (uname_len > width_uname)

                width_uname = uname_len;

            if (gname_len > width_gname)
                width_gname = gname_len;

            char num_buf[32];
            int num_len = snprintf(num_buf, sizeof(num_buf), "%lu", (unsigned long)st.st_nlink);

            if (num_len > 0 && (size_t)num_len > width_links)
                width_links = (size_t)num_len;

            num_len = snprintf(num_buf, sizeof(num_buf), "%llu", (unsigned long long)st.st_size);

            if (num_len > 0 && (size_t)num_len > width_size)
                width_size = (size_t)num_len;
        }
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return 1;
    }

    size_t cols = 1;
    size_t col_width = max_len + 2;

    if (!opt_long && !opt_single && col_width) {
        cols = term_width() / col_width;

        if (!cols)
            cols = 1;
    }

    size_t col = 0;
    while (getdents(fd, &ent) > 0) {
        char name[DIRENT_NAME_MAX];
        strncpy(name, ent.d_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';

        if (!want_name(name, opt_all, opt_almost))
            continue;

        if (opt_long) {
            char full[256];
            stat_t st;

            join_path(full, sizeof(full), path, name);

            if (stat(full, &st) < 0)
                memset(&st, 0, sizeof(st));

            char mode[11];
            format_mode(st.st_mode, mode);

            char line[256];
            char timebuf[32];
            format_time(st.st_mtime, timebuf, sizeof(timebuf));

            char uid_buf[16];
            char gid_buf[16];
            const char* uname = uid_name(st.st_uid, uid_buf, sizeof(uid_buf));
            const char* gname = gid_name(st.st_gid, gid_buf, sizeof(gid_buf));

            int len = snprintf(
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

            if (len < 0)
                continue;

            size_t out = (len < (int)sizeof(line)) ? (size_t)len : sizeof(line) - 1;
            write(STDOUT_FILENO, line, out);

            continue;
        }

        write_str(name);

        size_t name_len = strlen(name);

        if (opt_single || cols == 1) {
            write_char('\n');
            col = 0;
            continue;
        }

        size_t pad = col_width > name_len ? col_width - name_len : 1;
        for (size_t i = 0; i < pad; i++)
            write_char(' ');

        col++;

        if (col >= cols) {
            write_char('\n');
            col = 0;
        }
    }

    if (!opt_long && !opt_single && col != 0)
        write_char('\n');

    close(fd);
    return 0;
}

int main(int argc, char** argv) {
    bool opt_all = false;
    bool opt_almost = false;
    bool opt_long = false;
    bool opt_single = false;

    int argi = 1;
    for (; argi < argc; argi++) {
        const char* arg = argv[argi];

        if (!arg || arg[0] != '-' || !arg[1])
            break;

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
                write_str("ls: unknown option\n");
                return 1;
            }
        }
    }

    if (opt_long)
        opt_single = true;

    int paths = argc - argi;
    if (paths <= 0) {
        return list_dir(".", opt_all, opt_almost, opt_long, opt_single);
    }

    int status = 0;
    for (int i = argi; i < argc; i++) {
        if (paths > 1) {
            write_str(argv[i]);
            write_str(":\n");
        }

        if (list_dir(argv[i], opt_all, opt_almost, opt_long, opt_single) != 0)
            status = 1;

        if (paths > 1 && i + 1 < argc)
            write_char('\n');
    }

    return status;
}
