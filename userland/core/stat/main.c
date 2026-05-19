#include <account.h>
#include <fsutil.h>
#include <io.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(void) {
    io_write_str("usage: stat FILE...\n");
}

static int print_one(const char *path) {
    struct stat st;

    if (lstat(path, &st) < 0) {
        char line[256];
        snprintf(line, sizeof(line), "stat: cannot stat '%s'\n", path);
        io_write_str(line);
        return 1;
    }

    char mode_buf[11];
    char uid_buf[32];
    char gid_buf[32];
    char atime_buf[32];
    char mtime_buf[32];
    char ctime_buf[32];
    char line[512];

    fs_format_mode(st.st_mode, mode_buf);
    fs_format_time_short(st.st_atime, atime_buf, sizeof(atime_buf));
    fs_format_time_short(st.st_mtime, mtime_buf, sizeof(mtime_buf));
    fs_format_time_short(st.st_ctime, ctime_buf, sizeof(ctime_buf));

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
        (unsigned int)(st.st_mode & 07777),
        mode_buf,
        (unsigned long long)st.st_uid,
        account_uid_name(st.st_uid, uid_buf, sizeof(uid_buf)),
        (unsigned long long)st.st_gid,
        account_gid_name(st.st_gid, gid_buf, sizeof(gid_buf)),
        atime_buf,
        mtime_buf,
        ctime_buf
    );

    io_write_str(line);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    int rc = 0;

    for (int i = 1; i < argc; i++) {
        if (argc > 2 && i > 1) {
            io_write_str("\n");
        }

        if (print_one(argv[i]) != 0) {
            rc = 1;
        }
    }

    return rc;
}
