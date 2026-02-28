#include <account.h>
#include <ctype.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <user/kv.h>

#define GROUP_FILE_PATH "/etc/group"
#define GROUP_FILE_MAX  4096
#define ID_GROUP_MAX    32

static void format_identity(
    char *out,
    size_t out_len,
    const char *label,
    unsigned long long value,
    const char *name
) {
    if (!out || !out_len || !label) {
        return;
    }

    if (name && name[0]) {
        snprintf(out, out_len, "%s=%llu(%s)", label, value, name);
        return;
    }

    snprintf(out, out_len, "%s=%llu", label, value);
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

static size_t collect_groups(
    const char *user_name,
    gid_t primary_gid,
    gid_t *groups,
    size_t max_groups
) {
    if (!groups || !max_groups) {
        return 0;
    }

    size_t group_count = 0;
    append_group(primary_gid, groups, max_groups, &group_count);

    char group_file[GROUP_FILE_MAX];
    int group_fd = open(GROUP_FILE_PATH, O_RDONLY, 0);
    if (group_fd < 0) {
        return group_count;
    }

    ssize_t group_len = kv_read_fd(group_fd, group_file, sizeof(group_file));
    close(group_fd);
    if (group_len <= 0) {
        return group_count;
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

        bool include = ((gid_t)parsed_gid == primary_gid);
        if (!include) {
            include = member_list_has_user(members, user_name);
        }

        if (!include) {
            continue;
        }

        append_group((gid_t)parsed_gid, groups, max_groups, &group_count);
    }

    return group_count;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    uid_t uid = getuid();
    gid_t gid = getgid();

    char uname[32] = {0};
    char gname[32] = {0};

    char uid_part[64] = {0};
    char gid_part[64] = {0};
    char groups_part[320] = {0};
    char line[512] = {0};

    struct passwd *pwd = getpwuid(uid);
    const char *user_name =
        (pwd && pwd->pw_name && pwd->pw_name[0]) ? pwd->pw_name : "";

    gid_t groups[ID_GROUP_MAX] = {0};
    size_t group_count = collect_groups(
        user_name,
        gid,
        groups,
        sizeof(groups) / sizeof(groups[0])
    );

    format_identity(
        uid_part,
        sizeof(uid_part),
        "uid",
        (unsigned long long)uid,
        account_uid_name(uid, uname, sizeof(uname))
    );
    format_identity(
        gid_part,
        sizeof(gid_part),
        "gid",
        (unsigned long long)gid,
        account_gid_name(gid, gname, sizeof(gname))
    );

    size_t groups_used = 0;
    for (size_t i = 0; i < group_count; i++) {
        char group_name[32] = {0};
        const char *resolved = account_gid_name(
            groups[i],
            group_name,
            sizeof(group_name)
        );

        int n = snprintf(
            groups_part + groups_used,
            sizeof(groups_part) - groups_used,
            "%s%llu(%s)",
            i ? "," : "",
            (unsigned long long)groups[i],
            resolved
        );

        if (n <= 0 || groups_used + (size_t)n >= sizeof(groups_part)) {
            break;
        }

        groups_used += (size_t)n;
    }

    int written = 0;
    if (groups_used) {
        written = snprintf(
            line,
            sizeof(line),
            "%s %s groups=%s\n",
            uid_part,
            gid_part,
            groups_part
        );
    } else {
        written = snprintf(line, sizeof(line), "%s %s\n", uid_part, gid_part);
    }

    if (written > 0) {
        size_t len = (size_t)written;

        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }

        write(STDOUT_FILENO, line, len);
    }

    return 0;
}
