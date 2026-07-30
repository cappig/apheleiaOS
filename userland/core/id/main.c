#include <pwd.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <user/account.h>
#include <user/io.h>

#define ID_GROUP_MAX 32

// writes "<prefix><id>(<name>)", dropping the parentheses when the id has no name
static void print_id(const char *prefix, unsigned long long value, const char *name) {
    char text[64];

    if (name && name[0]) {
        snprintf(text, sizeof(text), "%s%llu(%s)", prefix, value, name);
    } else {
        snprintf(text, sizeof(text), "%s%llu", prefix, value);
    }

    io_write_str(text);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    uid_t uid = getuid();
    gid_t gid = getgid();
    char uname[32] = { 0 };
    char gname[32] = { 0 };

    struct passwd *pwd = getpwuid(uid);
    const char *user_name = (pwd && pwd->pw_name && pwd->pw_name[0]) ? pwd->pw_name : "";

    // the primary group leads, then the supplementary ones it is not part of
    gid_t groups[ID_GROUP_MAX] = { 0 };
    size_t count = 1;
    groups[0] = gid;
    count += account_groups(user_name, gid, groups + count, ID_GROUP_MAX - count);

    print_id("uid=", uid, account_uid_name(uid, uname, sizeof(uname)));
    print_id(" gid=", gid, account_gid_name(gid, gname, sizeof(gname)));

    for (size_t i = 0; i < count; i++) {
        char name[32] = { 0 };
        print_id(i ? "," : " groups=", groups[i], account_gid_name(groups[i], name, sizeof(name)));
    }

    io_write_char('\n');
    return 0;
}
