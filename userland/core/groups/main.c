#include <pwd.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <user/account.h>
#include <user/io.h>

#define GROUPS_MAX 32

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    gid_t gid = getgid();
    struct passwd *pwd = getpwuid(getuid());
    const char *user_name = (pwd && pwd->pw_name && pwd->pw_name[0]) ? pwd->pw_name : "";

    // the primary group leads, then the supplementary ones it is not part of
    gid_t groups[GROUPS_MAX] = { 0 };
    size_t count = 1;
    groups[0] = gid;
    count += account_groups(user_name, gid, groups + count, GROUPS_MAX - count);

    for (size_t i = 0; i < count; i++) {
        char name[32] = { 0 };

        if (i) {
            io_write_char(' ');
        }

        io_write_str(account_gid_name(groups[i], name, sizeof(name)));
    }

    io_write_char('\n');
    return 0;
}
