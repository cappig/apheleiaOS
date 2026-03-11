#include <ctype.h>
#include <crypt.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <user/kv.h>

extern char **environ;

static const char *group_file_path = "/etc/group";
#define SU_GROUP_MAX 16

static const char *env_lookup(const char *name) {
    if (!name || !name[0] || !environ) {
        return NULL;
    }

    size_t name_len = strlen(name);

    for (char **entry = environ; *entry; entry++) {
        if (!strncmp(*entry, name, name_len) && (*entry)[name_len] == '=') {
            return *entry + name_len + 1;
        }
    }

    return NULL;
}

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

static bool read_proc_uid(pid_t pid, uid_t *uid_out) {
    if (pid <= 0 || !uid_out) {
        return false;
    }

    char path[48];
    int path_len = snprintf(path, sizeof(path), "/proc/%ld/uid", (long)pid);
    if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
        return false;
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }

    char buf[32];
    ssize_t count = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (count <= 0) {
        return false;
    }

    buf[count] = '\0';

    char *end = NULL;
    long parsed = strtol(buf, &end, 10);
    if (end == buf || parsed < 0) {
        return false;
    }

    while (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t') {
        end++;
    }

    if (*end != '\0') {
        return false;
    }

    *uid_out = (uid_t)parsed;
    return true;
}

static uid_t invoking_uid(void) {
    uid_t uid = getuid();
    uid_t parent_uid = uid;

    pid_t ppid = getppid();
    if (ppid > 0 && read_proc_uid(ppid, &parent_uid)) {
        return parent_uid;
    }

    return uid;
}

static bool _trim_token(char **text) {
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

static bool _member_list_has_user(const char *members, const char *user_name) {
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
        if (_trim_token(&trimmed) && !strcmp(trimmed, user_name)) {
            return true;
        }

        token = strtok_r(NULL, ",", &save);
    }

    return false;
}

static bool _group_allows_user(
    const char *group_name,
    const char *user_name,
    gid_t user_gid
) {
    if (!group_name || !group_name[0] || !user_name || !user_name[0]) {
        return false;
    }

    char group_file[4096];
    int group_fd = open(group_file_path, O_RDONLY, 0);
    if (group_fd < 0) {
        return false;
    }

    ssize_t group_len = kv_read_fd(group_fd, group_file, sizeof(group_file));
    close(group_fd);
    if (group_len <= 0) {
        return false;
    }

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

        (void)passwd;

        if (!name || !gid_text || strcmp(name, group_name)) {
            continue;
        }

        char *gid_field = gid_text;
        if (!_trim_token(&gid_field)) {
            return false;
        }

        char *end = NULL;
        long parsed_gid = strtol(gid_field, &end, 10);
        if (end == gid_field || parsed_gid < 0) {
            return false;
        }

        while (*end && isspace((unsigned char)*end)) {
            end++;
        }

        if (*end != '\0') {
            return false;
        }

        if ((gid_t)parsed_gid == user_gid) {
            return true;
        }

        if (!members) {
            return false;
        }

        return _member_list_has_user(members, user_name);
    }

    return false;
}

static void _append_gid(gid_t gid, gid_t *groups, size_t *count, size_t max_groups) {
    if (!groups || !count || *count >= max_groups) {
        return;
    }

    for (size_t i = 0; i < *count; i++) {
        if (groups[i] == gid) {
            return;
        }
    }

    groups[*count] = gid;
    (*count)++;
}

static size_t _collect_supp_groups(
    const char *user_name,
    gid_t primary_gid,
    gid_t *groups,
    size_t max_groups
) {
    if (!user_name || !user_name[0] || !groups || !max_groups) {
        return 0;
    }

    char group_file[4096];
    int group_fd = open(group_file_path, O_RDONLY, 0);
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

        if (!gid_text || !members) {
            continue;
        }

        char *gid_field = gid_text;
        if (!_trim_token(&gid_field)) {
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

        if (!_member_list_has_user(members, user_name)) {
            continue;
        }

        _append_gid(gid, groups, &group_count, max_groups);
    }

    return group_count;
}

static bool _wheel_allows_uid(uid_t uid) {
    if (uid == 0) {
        return true;
    }

    struct passwd *caller = getpwuid(uid);
    if (!caller || !caller->pw_name || !caller->pw_name[0]) {
        return false;
    }

    return _group_allows_user("wheel", caller->pw_name, caller->pw_gid);
}

static bool authenticate_user(const char *name) {
    if (!name || !name[0]) {
        return false;
    }

    struct spwd *shadow = getspnam(name);
    if (!shadow || !shadow->sp_pwdp || !shadow->sp_pwdp[0]) {
        write_str("su: authentication failed\n");
        return false;
    }

    char pass[64] = {0};

    write_str("Password: ");
    (void)tty_set_echo(false);

    if (read_line(pass, sizeof(pass)) < 0) {
        (void)tty_set_echo(true);
        write_str("\n");
        return false;
    }

    (void)tty_set_echo(true);
    write_str("\n");

    strip_newline(pass);

    const char *hashed = crypt(pass, shadow->sp_pwdp);
    if (!hashed || strcmp(hashed, shadow->sp_pwdp)) {
        write_str("su: authentication failed\n");
        return false;
    }

    return true;
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

    struct passwd *pwd = getpwnam(target_name);
    if (!pwd) {
        write_str("su: unknown user\n");
        return 1;
    }

    uid_t caller_uid = invoking_uid();
    if (pwd->pw_uid == 0 && !_wheel_allows_uid(caller_uid)) {
        write_str("su: permission denied (wheel group required)\n");
        return 1;
    }

    if (caller_uid != 0) {
        if (!authenticate_user(target_name)) {
            return 1;
        }
    }

    gid_t groups[SU_GROUP_MAX] = {0};
    size_t group_count = _collect_supp_groups(
        pwd->pw_name,
        pwd->pw_gid,
        groups,
        sizeof(groups) / sizeof(groups[0])
    );

    if (
        setgroups(group_count, groups) < 0 ||
        setgid(pwd->pw_gid) < 0 ||
        setuid(pwd->pw_uid) < 0
    ) {
        write_str("su: failed to switch credentials\n");
        return 1;
    }

    if (pwd->pw_dir && pwd->pw_dir[0]) {
        (void)chdir(pwd->pw_dir);
    }

    const char *fallback_shell =
        (pwd->pw_shell && pwd->pw_shell[0]) ? pwd->pw_shell : "/bin/sh";
    const char *shell = env_lookup("SHELL");
    if (!shell || !shell[0]) {
        shell = fallback_shell;
    }

    char *shell_argv[] = {(char *)shell, NULL};
    execve(shell, shell_argv, environ);

    if (strcmp(shell, fallback_shell)) {
        shell_argv[0] = (char *)fallback_shell;
        execve(fallback_shell, shell_argv, environ);
    }

    if (strcmp(fallback_shell, "/bin/sh")) {
        shell_argv[0] = "/bin/sh";
        execve("/bin/sh", shell_argv, environ);
    }

    write_str("su: exec failed\n");
    return 1;
}
