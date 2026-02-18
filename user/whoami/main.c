#include <account.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    uid_t uid = getuid();
    char name[32] = {0};

    const char *value = account_uid_name(uid, name, sizeof(name));
    write(STDOUT_FILENO, value, strnlen(value, sizeof(name)));
    write(STDOUT_FILENO, "\n", 1);

    return 0;
}
