#include <errno.h>
#include <pwd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <user/account.h>

extern char **environ;

#define SU_GROUP_MAX 16

static ssize_t write_str(const char *str) {
    if (!str) {
        return 0;
    }

    return write(STDOUT_FILENO, str, strlen(str));
}

static void clear_secret(char *text, size_t len) {
    volatile char *dst = (volatile char *)text;

    while (len--) {
        *dst++ = '\0';
    }
}

static int read_line(char *buf, size_t len) {
    if (!buf || len < 2) {
        return -1;
    }

    size_t used = 0;
    while (used + 1 < len) {
        char ch = 0;
        ssize_t count = read(STDIN_FILENO, &ch, 1);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }
        if (ch == '\n' || ch == '\r') {
            buf[used] = '\0';
            return 0;
        }

        buf[used++] = ch;
    }

    for (;;) {
        char ch = 0;
        ssize_t count = read(STDIN_FILENO, &ch, 1);
        if (count > 0 && ch != '\n' && ch != '\r') {
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    buf[0] = '\0';
    return -1;
}

static bool tty_echo(bool enabled) {
    termios_t attr = { 0 };
    if (ioctl(STDIN_FILENO, TCGETS, &attr) < 0) {
        return false;
    }

    if (enabled) {
        attr.c_lflag |= ECHO;
    } else {
        attr.c_lflag &= ~ECHO;
    }

    return ioctl(STDIN_FILENO, TCSETS, &attr) == 0;
}

static bool authenticate(const char *name) {
    char password[256] = { 0 };

    write_str("password: ");
    if (!tty_echo(false)) {
        write_str("su: failed to configure terminal\n");
        return false;
    }

    int status = read_line(password, sizeof(password));
    bool echo_restored = tty_echo(true);
    write_str("\n");

    bool valid = status == 0 && echo_restored && account_auth(name, password);
    clear_secret(password, sizeof(password));
    return valid;
}

int main(int argc, char **argv) {
    const char *target_name = "root";

    if (argc > 2) {
        write_str("usage: su [user]\n");
        return 1;
    }

    if (argc == 2 && argv[1] && argv[1][0]) {
        target_name = argv[1];
    }

    uid_t caller_uid = getuid();
    if (geteuid() != 0) {
        write_str("su: must be installed setuid root\n");
        return 1;
    }

    struct passwd *pwd = getpwnam(target_name);
    if (!pwd) {
        write_str("su: unknown user\n");
        return 1;
    }

    if (caller_uid != 0 && !authenticate(pwd->pw_name)) {
        write_str("su: authentication failed\n");
        return 1;
    }

    const char *shell = (pwd->pw_shell && pwd->pw_shell[0]) ? pwd->pw_shell : "/bin/sh";
    if (!account_set_env(pwd, shell)) {
        write_str("su: failed to prepare environment\n");
        return 1;
    }

    gid_t groups[SU_GROUP_MAX] = { 0 };
    size_t group_count = account_groups(pwd->pw_name, pwd->pw_gid, groups, sizeof(groups) / sizeof(groups[0]));

    if (setgroups(group_count, groups) < 0) {
        write_str("su: failed to switch credentials\n");
        return 1;
    }

    if (setgid(pwd->pw_gid) < 0) {
        write_str("su: failed to switch credentials\n");
        return 1;
    }

    if (setuid(pwd->pw_uid) < 0) {
        write_str("su: failed to switch credentials\n");
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

    write_str("su: exec failed\n");
    return 1;
}
