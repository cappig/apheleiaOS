#include <ctype.h>
#include <crypt.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <user/kv.h>

#define GROUP_FILE_PATH "/etc/group"
#define GROUP_FILE_MAX  4096
#define LOGIN_GROUP_MAX 16

static ssize_t write_str(const char *str) {
    if (!str) {
        return 0;
    }

    return write(STDOUT_FILENO, str, strlen(str));
}

static void strip_newline(char *buf) {
    size_t len = strlen(buf);
    if (!len) {
        return;
    }

    if (buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
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

        if (count <= 0) {
            continue;
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

static bool member_list_has_user(const char *members, const char *user_name) {
    if (!members || !user_name || !user_name[0]) {
        return false;
    }

    char member_buf[256];
    size_t members_len = strlen(members);
    if (members_len >= sizeof(member_buf)) {
        members_len = sizeof(member_buf) - 1;
    }

    memcpy(member_buf, members, members_len);
    member_buf[members_len] = '\0';

    char *save = NULL;
    char *token = strtok_r(member_buf, ",", &save);
    while (token) {
        char *trimmed = token;
        if (trim_token(&trimmed) && !strcmp(trimmed, user_name)) {
            return true;
        }

        token = strtok_r(NULL, ",", &save);
    }

    return false;
}

static void append_group(
    gid_t gid,
    gid_t *groups,
    size_t max_groups,
    size_t *group_count
) {
    if (!groups || !group_count) {
        return;
    }

    for (size_t i = 0; i < *group_count; i++) {
        if (groups[i] == gid) {
            return;
        }
    }

    if (*group_count >= max_groups) {
        return;
    }

    groups[*group_count] = gid;
    (*group_count)++;
}

static size_t collect_supp_groups(
    const char *user_name,
    gid_t primary_gid,
    gid_t *groups,
    size_t max_groups
) {
    if (!groups || !max_groups) {
        return 0;
    }

    char group_file[GROUP_FILE_MAX];
    int group_fd = open(GROUP_FILE_PATH, O_RDONLY, 0);
    if (group_fd < 0) {
        return 0;
    }

    ssize_t group_len = kv_read_fd(group_fd, group_file, sizeof(group_file));
    close(group_fd);
    if (group_len <= 0) {
        return 0;
    }

    size_t group_count = 0;
    char *cursor = group_file;
    while (cursor && *cursor) {
        char *line = cursor;
        char *next = strchr(cursor, '\n');
        if (next) {
            *next = '\0';
            cursor = next + 1;
        } else {
            cursor = NULL;
        }

        while (*line && isspace((unsigned char)*line)) {
            line++;
        }

        if (!line[0] || line[0] == '#') {
            continue;
        }

        char line_buf[256];
        size_t line_len = strlen(line);
        if (line_len >= sizeof(line_buf)) {
            continue;
        }

        memcpy(line_buf, line, line_len + 1);

        char *save = NULL;
        char *name = strtok_r(line_buf, ":", &save);
        char *passwd = strtok_r(NULL, ":", &save);
        char *gid_text = strtok_r(NULL, ":", &save);
        char *members = strtok_r(NULL, ":", &save);

        (void)name;
        (void)passwd;

        if (!gid_text) {
            continue;
        }

        char *gid_field = gid_text;
        if (!trim_token(&gid_field)) {
            continue;
        }

        char *end = NULL;
        long parsed_gid = strtol(gid_field, &end, 10);
        if (end == gid_field || parsed_gid < 0) {
            continue;
        }

        while (*end && isspace((unsigned char)*end)) {
            end++;
        }

        if (*end != '\0') {
            continue;
        }

        gid_t gid = (gid_t)parsed_gid;
        if (gid == primary_gid) {
            continue;
        }

        if (!member_list_has_user(members, user_name)) {
            continue;
        }

        append_group(gid, groups, max_groups, &group_count);
    }

    return group_count;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    for (;;) {
        char name[32] = {0};
        char pass[64] = {0};

        pid_t pid = getpid();
        ioctl(STDIN_FILENO, TIOCSPGRP, &pid);

        write_str("login: ");
        if (read_line(name, sizeof(name)) < 0) {
            continue;
        }

        strip_newline(name);

        if (!name[0]) {
            continue;
        }

        struct passwd *pwd = getpwnam(name);
        if (!pwd) {
            write_str("login: unknown user\n");
            continue;
        }

        struct spwd *shadow = getspnam(name);
        if (!shadow) {
            write_str("login: authentication failed\n");
            continue;
        }

        write_str("Password: ");
        tty_set_echo(false);

        if (read_line(pass, sizeof(pass)) < 0) {
            tty_set_echo(true);
            write_str("\n");
            continue;
        }

        tty_set_echo(true);
        write_str("\n");

        strip_newline(pass);

        const char *hashed = crypt(pass, shadow->sp_pwdp);

        if (!hashed || strcmp(hashed, shadow->sp_pwdp)) {
            write_str("login: authentication failed\n");
            continue;
        }

        gid_t groups[LOGIN_GROUP_MAX] = {0};
        size_t group_count = collect_supp_groups(
            name,
            pwd->pw_gid,
            groups,
            sizeof(groups) / sizeof(groups[0])
        );

        if (
            setgroups(group_count, groups) < 0 ||
            setgid(pwd->pw_gid) < 0 ||
            setuid(pwd->pw_uid) < 0
        ) {
            write_str("login: failed to set credentials\n");
            continue;
        }

        if (pwd->pw_dir && pwd->pw_dir[0]) {
            chdir(pwd->pw_dir);
        }

        const char *shell =
            (pwd->pw_shell && pwd->pw_shell[0]) ? pwd->pw_shell : "/bin/sh";

        char *args[] = {(char *)shell, NULL};
        execve(shell, args, NULL);

        write_str("login: exec failed\n");
        _exit(1);
    }
}
