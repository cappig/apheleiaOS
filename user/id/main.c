#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

static void format_identity(
    char* out,
    size_t out_len,
    const char* label,
    unsigned long long value,
    const char* name
) {
    if (!out || !out_len || !label)
        return;

    if (name && name[0]) {
        snprintf(out, out_len, "%s=%llu(%s)", label, value, name);
        return;
    }

    snprintf(out, out_len, "%s=%llu", label, value);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    uid_t uid = getuid();
    gid_t gid = getgid();

    passwd_t pwd = {0};
    group_t grp = {0};

    const char* uname = NULL;
    const char* gname = NULL;

    if (!getpwuid(uid, &pwd))
        uname = pwd.pw_name;

    if (!getgrgid(gid, &grp))
        gname = grp.gr_name;

    char uid_part[64] = {0};
    char gid_part[64] = {0};
    char line[160] = {0};

    format_identity(uid_part, sizeof(uid_part), "uid", (unsigned long long)uid, uname);
    format_identity(gid_part, sizeof(gid_part), "gid", (unsigned long long)gid, gname);

    int written = snprintf(line, sizeof(line), "%s %s\n", uid_part, gid_part);

    if (written > 0) {
        size_t len = (size_t)written;

        if (len >= sizeof(line))
            len = sizeof(line) - 1;

        write(STDOUT_FILENO, line, len);
    }

    return 0;
}
