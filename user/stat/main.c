#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ssize_t write_str(const char* str) {
    if (!str)
        return 0;

    return write(STDOUT_FILENO, str, strlen(str));
}

static void usage(void) {
    write_str("usage: stat FILE...\n");
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

static int print_one(const char* path) {
    stat_t st;
    if (lstat(path, &st) < 0) {
        char line[256];
        snprintf(line, sizeof(line), "stat: cannot stat '%s'\n", path);
        write_str(line);
        return 1;
    }

    char mode_buf[11];
    char uid_buf[32];
    char gid_buf[32];
    char atime_buf[32];
    char mtime_buf[32];
    char ctime_buf[32];
    char line[512];

    format_mode(st.st_mode, mode_buf);
    format_time(st.st_atime, atime_buf, sizeof(atime_buf));
    format_time(st.st_mtime, mtime_buf, sizeof(mtime_buf));
    format_time(st.st_ctime, ctime_buf, sizeof(ctime_buf));

    snprintf(
        line,
        sizeof(line),
        "  File: %s\n"
        "  Size: %llu\tLinks: %llu\tInode: %llu\n"
        "Access: (%04o/%s)\tUid: (%llu/%s)\tGid: (%llu/%s)\n"
        "Access: %s\n"
        "Modify: %s\n"
        "Change: %s\n",
        path,
        (unsigned long long)st.st_size,
        (unsigned long long)st.st_nlink,
        (unsigned long long)st.st_ino,
        st.st_mode & 07777,
        mode_buf,
        (unsigned long long)st.st_uid,
        uid_name(st.st_uid, uid_buf, sizeof(uid_buf)),
        (unsigned long long)st.st_gid,
        gid_name(st.st_gid, gid_buf, sizeof(gid_buf)),
        atime_buf,
        mtime_buf,
        ctime_buf
    );

    write_str(line);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argc > 2 && i > 1)
            write_str("\n");
        if (print_one(argv[i]) != 0)
            rc = 1;
    }

    return rc;
}
