#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    uid_t uid = getuid();
    passwd_t pwd = {0};

    if (!getpwuid(uid, &pwd) && pwd.pw_name[0]) {
        write(STDOUT_FILENO, pwd.pw_name, strlen(pwd.pw_name));
        write(STDOUT_FILENO, "\n", 1);
        return 0;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)uid);

    if (len < 0)
        return 1;

    size_t out = (len < (int)sizeof(buf)) ? (size_t)len : sizeof(buf) - 1;

    write(STDOUT_FILENO, buf, out);
    write(STDOUT_FILENO, "\n", 1);

    return 0;
}
