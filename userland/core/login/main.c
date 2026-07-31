#include <ctype.h>
#include <pwd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <user/account.h>
#include <user/io.h>

extern char **environ;

#define LOGIN_GROUP_MAX 16

static bool trim_token(char **text) {
    if (!text || !*text) {
        return false;
    }

    char *start = *text;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }

    *end = '\0';
    *text = start;
    return start[0] != '\0';
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    for (;;) {
        char name[32] = { 0 };

        pid_t pid = getpid();
        ioctl(STDIN_FILENO, TIOCSPGRP, &pid);

        io_write_str("login: ");
        ssize_t name_len = io_read_line(STDIN_FILENO, name, sizeof(name));
        if (name_len == -2) {
            io_write_str("login: name too long\n");
            continue;
        }
        if (name_len < 0) {
            return 1;
        }

        char *login_name = name;
        if (!trim_token(&login_name)) {
            continue;
        }

        // unknown names still get a prompt, so this cannot enumerate accounts
        struct passwd *pwd = getpwnam(login_name);
        bool authenticated = account_verify_password("password: ", login_name) && pwd;
        if (!authenticated) {
            io_write_str("login: authentication failed\n");
            continue;
        }

        gid_t groups[LOGIN_GROUP_MAX] = { 0 };
        size_t group_count = account_groups(login_name, pwd->pw_gid, groups, sizeof(groups) / sizeof(groups[0]));

        // never return to the prompt holding half of the new credentials
        if (setgroups(group_count, groups) < 0 || setgid(pwd->pw_gid) < 0 || setuid(pwd->pw_uid) < 0) {
            io_write_str("login: failed to set credentials\n");
            _exit(1);
        }

        if (pwd->pw_dir && pwd->pw_dir[0]) {
            chdir(pwd->pw_dir);
        }

        const char *shell = (pwd->pw_shell && pwd->pw_shell[0]) ? pwd->pw_shell : "/bin/sh";
        if (!account_set_env(pwd, shell)) {
            io_write_str("login: failed to prepare environment\n");
            _exit(1);
        }

        char *args[] = { (char *)shell, NULL };
        execve(shell, args, environ);

        io_write_str("login: exec failed\n");
        _exit(1);
    }
}
