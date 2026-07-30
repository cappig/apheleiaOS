#include <pwd.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <user/account.h>
#include <user/io.h>

extern char **environ;

#define SU_GROUP_MAX 16

int main(int argc, char **argv) {
    const char *target_name = "root";

    if (argc > 2) {
        io_write_str("usage: su [user]\n");
        return 1;
    }

    if (argc == 2 && argv[1] && argv[1][0]) {
        target_name = argv[1];
    }

    uid_t caller_uid = getuid();
    if (geteuid() != 0) {
        io_write_str("su: must be installed setuid root\n");
        return 1;
    }

    struct passwd *pwd = getpwnam(target_name);
    if (!pwd) {
        io_write_str("su: unknown user\n");
        return 1;
    }

    if (caller_uid != 0 && !account_verify_password("password: ", pwd->pw_name)) {
        io_write_str("su: authentication failed\n");
        return 1;
    }

    const char *shell = (pwd->pw_shell && pwd->pw_shell[0]) ? pwd->pw_shell : "/bin/sh";
    if (!account_set_env(pwd, shell)) {
        io_write_str("su: failed to prepare environment\n");
        return 1;
    }

    gid_t groups[SU_GROUP_MAX] = { 0 };
    size_t group_count = account_groups(pwd->pw_name, pwd->pw_gid, groups, sizeof(groups) / sizeof(groups[0]));

    if (setgroups(group_count, groups) < 0) {
        io_write_str("su: failed to switch credentials\n");
        return 1;
    }

    if (setgid(pwd->pw_gid) < 0) {
        io_write_str("su: failed to switch credentials\n");
        return 1;
    }

    if (setuid(pwd->pw_uid) < 0) {
        io_write_str("su: failed to switch credentials\n");
        return 1;
    }

    if (pwd->pw_dir && pwd->pw_dir[0]) {
        (void)chdir(pwd->pw_dir);
    }

    char *shell_argv[] = { (char *)shell, NULL };
    execve(shell, shell_argv, environ);

    if (strcmp(shell, "/bin/sh")) {
        shell_argv[0] = "/bin/sh";
        execve("/bin/sh", shell_argv, environ);
    }

    io_write_str("su: exec failed\n");
    return 1;
}
