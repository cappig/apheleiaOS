#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    gid_t gid = getgid();
    group_t grp = {0};

    if (!getgrgid(gid, &grp) && grp.gr_name[0]) {
        write(STDOUT_FILENO, grp.gr_name, strlen(grp.gr_name));
        write(STDOUT_FILENO, "\n", 1);
        return 0;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)gid);

    if (len < 0)
        return 1;

    size_t out = (len < (int)sizeof(buf)) ? (size_t)len : sizeof(buf) - 1;

    write(STDOUT_FILENO, buf, out);
    write(STDOUT_FILENO, "\n", 1);

    return 0;
}
