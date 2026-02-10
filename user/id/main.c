#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

static size_t append_unsigned(char* buf, size_t size, unsigned long long value) {
    if (size == 0)
        return 0;

    int written = snprintf(buf, size, "%llu", value);
    if (written < 0)
        return 0;

    if ((size_t)written >= size)
        return size - 1;

    return (size_t)written;
}

static size_t append_uid(char* buf, size_t size, uid_t uid, const char* name) {
    size_t pos = 0;

    if (size == 0)
        return 0;

    const char prefix[] = "uid=";
    for (size_t i = 0; i < sizeof(prefix) - 1 && pos + 1 < size; i++)
        buf[pos++] = prefix[i];

    pos += append_unsigned(buf + pos, size - pos, (unsigned long long)uid);

    if (name && name[0]) {
        if (pos + 1 < size)
            buf[pos++] = '(';
        for (size_t i = 0; name[i] && pos + 1 < size; i++)
            buf[pos++] = name[i];
        if (pos + 1 < size)
            buf[pos++] = ')';
    }

    return pos;
}

static size_t append_gid(char* buf, size_t size, gid_t gid, const char* name) {
    size_t pos = 0;

    if (size == 0)
        return 0;

    const char prefix[] = "gid=";
    for (size_t i = 0; i < sizeof(prefix) - 1 && pos + 1 < size; i++)
        buf[pos++] = prefix[i];

    pos += append_unsigned(buf + pos, size - pos, (unsigned long long)gid);

    if (name && name[0]) {
        if (pos + 1 < size)
            buf[pos++] = '(';
        for (size_t i = 0; name[i] && pos + 1 < size; i++)
            buf[pos++] = name[i];
        if (pos + 1 < size)
            buf[pos++] = ')';
    }

    return pos;
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

    if (getpwuid(uid, &pwd) == 0)
        uname = pwd.pw_name;
    if (getgrgid(gid, &grp) == 0)
        gname = grp.gr_name;

    char buf[128];
    size_t pos = 0;

    pos += append_uid(buf + pos, sizeof(buf) - pos, uid, uname);
    if (pos + 1 < sizeof(buf))
        buf[pos++] = ' ';
    pos += append_gid(buf + pos, sizeof(buf) - pos, gid, gname);

    if (pos + 1 < sizeof(buf))
        buf[pos++] = '\n';

    write(STDOUT_FILENO, buf, pos);
    return 0;
}
