#include "complete.h"

#include <dirent.h>
#include <fcntl.h>
#include <io.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <fsutil.h>

#define SH_MATCH_MAX 128
#define SH_PATH_MAX  1024

typedef struct {
    char name[DIRENT_NAME_MAX];
    bool is_dir;
} sh_match_t;

static char sh_complete_path[SH_PATH_MAX] = "/bin";

static const char* sh_builtins[] = {
    "bg",
    "cd",
    "echo",
    "env",
    "exit",
    "fg",
    "help",
    "history",
    "jobs",
    "set",
    "umask",
    "unset",
    NULL,
};

void complete_set_path(const char* path) {
    if (!path || !path[0]) {
        snprintf(sh_complete_path, sizeof(sh_complete_path), "/bin");
        return;
    }

    snprintf(sh_complete_path, sizeof(sh_complete_path), "%s", path);
}

static bool is_word_delim(char ch) {
    return ch == '\0' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '|' || ch == '&' ||
           ch == '<' || ch == '>' || ch == ';';
}

static bool is_command_position(const char* buf, size_t token_start) {
    if (!buf)
        return false;

    size_t i = token_start;
    while (i > 0 && (buf[i - 1] == ' ' || buf[i - 1] == '\t'))
        i--;

    if (!i)
        return true;

    char ch = buf[i - 1];
    return ch == '|' || ch == ';' || ch == '&';
}

static bool starts_with(const char* text, const char* prefix) {
    if (!text || !prefix)
        return false;

    while (*prefix) {
        if (*text++ != *prefix++)
            return false;
    }

    return true;
}

static size_t completion_term_cols(void) {
    winsize_t ws = {0};
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_col)
        return (size_t)ws.ws_col;

    return 80;
}

static int match_name_cmp(const void* lhs, const void* rhs) {
    const sh_match_t* a = (const sh_match_t*)lhs;
    const sh_match_t* b = (const sh_match_t*)rhs;
    return strcmp(a->name, b->name);
}

static size_t match_display_len(const sh_match_t* match) {
    if (!match)
        return 0;

    return strlen(match->name) + (match->is_dir ? 1 : 0);
}

static size_t lcp_len(const sh_match_t* matches, size_t count) {
    if (!matches || !count)
        return 0;

    size_t len = strlen(matches[0].name);
    for (size_t i = 1; i < count; i++) {
        size_t j = 0;
        const char* a = matches[0].name;
        const char* b = matches[i].name;

        while (j < len && b[j] && a[j] == b[j])
            j++;

        len = j;
    }

    return len;
}

static bool match_is_dir(const char* dir_path, const char* name) {
    if (!dir_path || !name)
        return false;

    char full[SH_PATH_MAX];
    fs_join_path(full, sizeof(full), dir_path, name);

    struct stat st;
    if (stat(full, &st) < 0)
        return false;

    return (st.st_mode & S_IFMT) == S_IFDIR;
}

static bool command_is_runnable(const char* dir_path, const char* name) {
    if (!dir_path || !name)
        return false;

    char full[SH_PATH_MAX];
    fs_join_path(full, sizeof(full), dir_path, name);

    struct stat st;
    if (stat(full, &st) < 0)
        return false;

    if ((st.st_mode & S_IFMT) == S_IFDIR)
        return false;

    return !access(full, X_OK);
}

static bool add_match(sh_match_t* matches, size_t* count, size_t cap, const char* name, bool is_dir) {
    if (!matches || !count || !name || !name[0])
        return false;

    for (size_t i = 0; i < *count; i++) {
        if (strcmp(matches[i].name, name))
            continue;

        if (is_dir)
            matches[i].is_dir = true;

        return true;
    }

    if (*count >= cap)
        return false;

    sh_match_t* match = &matches[(*count)++];
    snprintf(match->name, sizeof(match->name), "%s", name);
    match->is_dir = is_dir;

    return true;
}

static size_t collect_matches(
    const char* dir_path,
    const char* prefix,
    bool include_hidden,
    sh_match_t* matches,
    size_t cap
) {
    if (!dir_path || !prefix || !matches || !cap)
        return 0;

    int fd = open(dir_path, O_RDONLY, 0);
    if (fd < 0)
        return 0;

    size_t count = 0;
    dirent_t dent;

    while (getdents(fd, &dent) > 0) {
        if (!dent.d_name[0])
            continue;

        if (!strcmp(dent.d_name, ".") || !strcmp(dent.d_name, ".."))
            continue;

        if (!include_hidden && dent.d_name[0] == '.')
            continue;

        if (!starts_with(dent.d_name, prefix))
            continue;

        add_match(matches, &count, cap, dent.d_name, match_is_dir(dir_path, dent.d_name));
    }

    close(fd);
    return count;
}

static size_t collect_command_matches(
    const char* prefix,
    bool include_hidden,
    sh_match_t* matches,
    size_t cap
) {
    if (!prefix || !matches || !cap)
        return 0;

    char path_buf[SH_PATH_MAX];
    snprintf(path_buf, sizeof(path_buf), "%s", sh_complete_path);

    size_t count = 0;

    for (const char** bp = sh_builtins; *bp; bp++) {
        if (!starts_with(*bp, prefix))
            continue;

        add_match(matches, &count, cap, *bp, false);
    }

    char* cursor = path_buf;

    while (cursor && *cursor) {
        char* next = strchr(cursor, ':');

        if (next)
            *next = '\0';

        const char* dir = cursor[0] ? cursor : ".";
        int fd = open(dir, O_RDONLY, 0);

        if (fd >= 0) {
            dirent_t dent;
            while (getdents(fd, &dent) > 0) {
                if (!dent.d_name[0])
                    continue;

                if (!strcmp(dent.d_name, ".") || !strcmp(dent.d_name, ".."))
                    continue;

                if (!include_hidden && dent.d_name[0] == '.')
                    continue;

                if (!starts_with(dent.d_name, prefix))
                    continue;

                if (!command_is_runnable(dir, dent.d_name))
                    continue;

                add_match(matches, &count, cap, dent.d_name, false);
            }

            close(fd);
        }

        if (!next)
            break;

        cursor = next + 1;
    }

    return count;
}

static void list_matches(sh_match_t* matches, size_t count) {
    if (!matches || !count)
        return;

    qsort(matches, count, sizeof(matches[0]), match_name_cmp);

    size_t max_len = 0;

    for (size_t i = 0; i < count; i++) {
        size_t width = match_display_len(&matches[i]);

        if (width > max_len)
            max_len = width;
    }

    size_t cols = completion_term_cols();

    size_t cell = max_len + 2;
    if (!cell)
        cell = 1;

    size_t per_row = cols / cell;
    if (!per_row)
        per_row = 1;

    io_write_str("\n");

    for (size_t i = 0; i < count; i++) {
        char item[DIRENT_NAME_MAX + 2];
        snprintf(item, sizeof(item), "%s%s", matches[i].name, matches[i].is_dir ? "/" : "");
        io_write_str(item);

        bool end_row = ((i + 1) % per_row) == 0 || i + 1 == count;
        if (end_row) {
            io_write_str("\n");
            continue;
        }

        size_t pad = cell - strlen(item);
        while (pad-- > 0)
            io_write_str(" ");
    }
}

static bool build_candidate(
    char* out,
    size_t out_len,
    const char* typed_dir,
    const char* name,
    bool is_dir
) {
    if (!out || !out_len || !typed_dir || !name)
        return false;

    int rc;
    if (is_dir)
        rc = snprintf(out, out_len, "%s%s/", typed_dir, name);
    else
        rc = snprintf(out, out_len, "%s%s", typed_dir, name);

    return rc >= 0 && (size_t)rc < out_len;
}

static void split_prefix(
    const char* token,
    size_t token_len,
    char* dir_open,
    size_t dir_open_len,
    char* typed_dir,
    size_t typed_dir_len,
    char* base_prefix,
    size_t base_prefix_len
) {
    if (!token || !dir_open || !typed_dir || !base_prefix)
        return;

    ssize_t slash = -1;

    for (size_t i = 0; i < token_len; i++) {
        if (token[i] == '/')
            slash = (ssize_t)i;
    }

    if (slash < 0) {
        snprintf(dir_open, dir_open_len, ".");

        if (typed_dir_len)
            typed_dir[0] = '\0';

        snprintf(base_prefix, base_prefix_len, "%.*s", (int)token_len, token);
        return;
    }

    size_t typed_len = (size_t)slash + 1;

    snprintf(typed_dir, typed_dir_len, "%.*s", (int)typed_len, token);
    snprintf(base_prefix, base_prefix_len, "%.*s", (int)(token_len - typed_len), token + typed_len);

    if (!slash) {
        snprintf(dir_open, dir_open_len, "/");
    } else {
        snprintf(dir_open, dir_open_len, "%.*s", (int)slash, token);
    }
}

void complete_line(
    char* buf,
    size_t cap,
    size_t* len,
    size_t* cursor,
    sh_complete_result_t* result
) {
    if (!buf || !cap || !len || !cursor || *cursor > *len)
        return;

    if (result)
        memset(result, 0, sizeof(*result));

    size_t token_start = *cursor;
    while (token_start > 0 && !is_word_delim(buf[token_start - 1]))
        token_start--;

    if (*cursor == token_start)
        return;

    char token[SH_PATH_MAX];
    size_t token_len = *cursor - token_start;

    if (token_len + 1 > sizeof(token))
        return;

    memcpy(token, buf + token_start, token_len);
    token[token_len] = '\0';

    sh_match_t* matches = malloc(SH_MATCH_MAX * sizeof(sh_match_t));
    if (!matches)
        return;
    bool command_mode = !strchr(token, '/') && is_command_position(buf, token_start);
    bool include_hidden = token[0] == '.';
    size_t match_count = 0;

    char dir_open[SH_PATH_MAX] = {0};
    char typed_dir[SH_PATH_MAX] = {0};
    char base_prefix[SH_PATH_MAX] = {0};

    if (command_mode) {
        match_count = collect_command_matches(token, include_hidden, matches, SH_MATCH_MAX);
    } else {
        split_prefix(
            token,
            token_len,
            dir_open,
            sizeof(dir_open),
            typed_dir,
            sizeof(typed_dir),
            base_prefix,
            sizeof(base_prefix)
        );
        include_hidden = base_prefix[0] == '.';
        match_count = collect_matches(dir_open, base_prefix, include_hidden, matches, SH_MATCH_MAX);
    }

    if (!match_count) {
        free(matches);
        return;
    }

    char candidate[SH_PATH_MAX];
    bool have_candidate = false;

    if (match_count == 1) {
        if (command_mode) {
            int rc = snprintf(candidate, sizeof(candidate), "%s", matches[0].name);
            have_candidate = rc >= 0 && (size_t)rc < sizeof(candidate);
        } else {
            have_candidate = build_candidate(
                candidate,
                sizeof(candidate),
                typed_dir,
                matches[0].name,
                matches[0].is_dir
            );
        }
    } else {
        size_t common = lcp_len(matches, match_count);
        size_t base_len = command_mode ? token_len : strlen(base_prefix);

        if (common > base_len) {
            char common_name[DIRENT_NAME_MAX];
            snprintf(common_name, sizeof(common_name), "%.*s", (int)common, matches[0].name);

            if (command_mode) {
                int rc = snprintf(candidate, sizeof(candidate), "%s", common_name);
                have_candidate = rc >= 0 && (size_t)rc < sizeof(candidate);
            } else {
                have_candidate =
                    build_candidate(candidate, sizeof(candidate), typed_dir, common_name, false);
            }
        } else {
            list_matches(matches, match_count);

            if (result)
                result->listed = true;
            free(matches);
            return;
        }
    }

    if (!have_candidate) {
        free(matches);
        return;
    }

    size_t candidate_len = strlen(candidate);
    size_t tail_len = *len - *cursor;
    size_t replaced_len = *cursor - token_start;
    size_t new_len = *len - replaced_len + candidate_len;

    if (new_len + 1 > cap)
        return;

    memmove(buf + token_start + candidate_len, buf + *cursor, tail_len + 1);
    memcpy(buf + token_start, candidate, candidate_len);

    size_t old_cursor = *cursor;
    *len = new_len;
    *cursor = token_start + candidate_len;

    if (result) {
        result->changed = true;

        if (candidate_len > token_len && !strncmp(candidate, token, token_len)) {
            result->erase_valid = true;
            result->erase_start = old_cursor;
            result->erase_end = old_cursor + (candidate_len - token_len);
        }
    }

    free(matches);
}
