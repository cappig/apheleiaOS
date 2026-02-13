#include <account.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    gid_t gid = getgid();
    char name[32] = {0};

    const char* value = account_gid_name(gid, name, sizeof(name));

    write(STDOUT_FILENO, value, strnlen(value, sizeof(name)));
    write(STDOUT_FILENO, "\n", 1);

    return 0;
}
