#include <crypt.h>
#include <pwd.h>
#include <shadow.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static ssize_t write_str(const char* str) {
    if (!str)
        return 0;

    return write(STDOUT_FILENO, str, strlen(str));
}

static void strip_newline(char* buf) {
    size_t len = strlen(buf);
    if (!len)
        return;

    if (buf[len - 1] == '\n')
        buf[len - 1] = '\0';
}

static int read_line(char* buf, size_t len) {
    if (!buf || !len)
        return -1;

    size_t pos = 0;
    bool cr_seen = false;

    while (pos + 1 < len) {
        char ch = 0;
        ssize_t count = read(STDIN_FILENO, &ch, 1);
        if (count <= 0)
            continue;

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
        if (ch == '\n')
            break;
    }

    buf[pos] = '\0';
    return 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    for (;;) {
        char name[32] = {0};
        char pass[64] = {0};

        write_str("login: ");
        if (read_line(name, sizeof(name)) < 0)
            continue;

        strip_newline(name);

        if (!name[0])
            continue;

        passwd_t pwd = {0};
        if (getpwnam(name, &pwd) < 0) {
            write_str("login: unknown user\n");
            continue;
        }

        shadow_t shadow = {0};
        if (getspnam(name, &shadow) < 0) {
            write_str("login: authentication failed\n");
            continue;
        }

        write_str("Password: ");
        if (read_line(pass, sizeof(pass)) < 0)
            continue;

        strip_newline(pass);

        const char* hashed = crypt(pass, shadow.sp_pwd);
        if (!hashed || strcmp(hashed, shadow.sp_pwd) != 0) {
            write_str("login: authentication failed\n");
            continue;
        }

        if (setgid(pwd.pw_gid) < 0 || setuid(pwd.pw_uid) < 0) {
            write_str("login: failed to set credentials\n");
            continue;
        }

        if (pwd.pw_dir[0])
            chdir(pwd.pw_dir);

        const char* shell = pwd.pw_shell[0] ? pwd.pw_shell : "/sbin/sh";
        char* args[] = {(char*)shell, NULL};
        execve(shell, args, NULL);

        write_str("login: exec failed\n");
        _exit(1);
    }
}
