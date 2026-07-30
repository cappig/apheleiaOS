#include <ctype.h>
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

#define LOGIN_GROUP_MAX 16

static ssize_t write_str(const char *str) {
    if (!str) {
        return 0;
    }

    return write(STDOUT_FILENO, str, strlen(str));
}

static void strip_newline(char *buf) {
    size_t len = strlen(buf);

    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[len - 1] = '\0';
        len--;
    }
}

static void clear_secret(char *text, size_t len) {
    volatile char *dst = (volatile char *)text;

    while (len--) {
        *dst++ = '\0';
    }
}

static int read_line(char *buf, size_t len) {
    if (!buf || !len) {
        return -1;
    }

    size_t pos = 0;
    bool cr_seen = false;

    while (pos + 1 < len) {
        char ch = 0;
        ssize_t count = read(STDIN_FILENO, &ch, 1);

        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (!count) {
            if (pos) {
                break;
            }
            return -1;
        }

        if (count < 0) {
            return -1;
        }

        if (ch == '\r') {
            ch = '\n';
            cr_seen = true;
        } else if (ch == '\n' && cr_seen) {
            cr_seen = false;
            continue;
        } else {
            cr_seen = false;
        }

        buf[pos++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    if (pos + 1 >= len && (!pos || buf[pos - 1] != '\n')) {
        for (;;) {
            char ch = 0;
            ssize_t count = read(STDIN_FILENO, &ch, 1);
            if (count > 0) {
                if (ch == '\n' || ch == '\r') {
                    break;
                }
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        buf[0] = '\0';
        return -2;
    }

    buf[pos] = '\0';
    return 0;
}

static bool tty_set_echo(bool enable) {
    termios_t tos;

    if (ioctl(STDIN_FILENO, TCGETS, &tos) < 0) {
        return false;
    }

    if (enable) {
        tos.c_lflag |= ECHO;
    } else {
        tos.c_lflag &= ~ECHO;
    }

    return !ioctl(STDIN_FILENO, TCSETS, &tos);
}

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
        char pass[256] = { 0 };

        pid_t pid = getpid();
        ioctl(STDIN_FILENO, TIOCSPGRP, &pid);

        write_str("login: ");
        int name_status = read_line(name, sizeof(name));
        if (name_status == -2) {
            write_str("login: name too long\n");
            continue;
        }
        if (name_status < 0) {
            return 1;
        }

        strip_newline(name);

        char *login_name = name;
        if (!trim_token(&login_name)) {
            continue;
        }

        // an unknown name still gets a password prompt and the same failure
        // message, so the prompt cannot be used to enumerate accounts
        struct passwd *pwd = getpwnam(login_name);

        write_str("password: ");
        if (!tty_set_echo(false)) {
            write_str("login: failed to configure terminal\n");
            return 1;
        }

        int pass_status = read_line(pass, sizeof(pass));
        if (pass_status < 0) {
            tty_set_echo(true);
            write_str("\n");
            if (pass_status == -2) {
                write_str("login: password too long\n");
                continue;
            }
            return 1;
        }

        if (!tty_set_echo(true)) {
            write_str("\nlogin: failed to restore terminal\n");
            return 1;
        }

        write_str("\n");

        strip_newline(pass);

        bool authenticated = pwd && account_auth(login_name, pass);
        clear_secret(pass, sizeof(pass));
        if (!authenticated) {
            write_str("login: authentication failed\n");
            continue;
        }

        gid_t groups[LOGIN_GROUP_MAX] = { 0 };
        size_t group_count = account_groups(login_name, pwd->pw_gid, groups, sizeof(groups) / sizeof(groups[0]));

        // a half applied credential change must never fall back to the prompt:
        // give up and let init respawn a fresh login on this terminal
        if (setgroups(group_count, groups) < 0 || setgid(pwd->pw_gid) < 0 || setuid(pwd->pw_uid) < 0) {
            write_str("login: failed to set credentials\n");
            _exit(1);
        }

        if (pwd->pw_dir && pwd->pw_dir[0]) {
            chdir(pwd->pw_dir);
        }

        const char *shell = (pwd->pw_shell && pwd->pw_shell[0]) ? pwd->pw_shell : "/bin/sh";
        if (!account_set_env(pwd, shell)) {
            write_str("login: failed to prepare environment\n");
            _exit(1);
        }

        char *args[] = { (char *)shell, NULL };
        execve(shell, args, environ);

        write_str("login: exec failed\n");
        _exit(1);
    }
}
