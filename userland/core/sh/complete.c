#include "complete.h"

#include <dirent.h>
#include <fsutil.h>
#include <io.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <term_size.h>
#include <termios.h>
#include <unistd.h>

#define SH_MATCH_MAX 128
#define SH_PATH_MAX  1024

#define SH_C_RESET "\x1b[0m"
#define SH_C_BLUE  "\x1b[34m"

typedef struct {
    char name[NAME_MAX + 1];
    bool is_dir;
} sh_match_t;

typedef struct {
    char path[SH_PATH_MAX];
    bool color;
} sh_complete_state_t;

typedef struct {
    size_t token_start;
    size_t token_len;
    bool command;

    char token[SH_PATH_MAX];
    char dir_open[SH_PATH_MAX];
    char typed_dir[SH_PATH_MAX];
    char base_prefix[SH_PATH_MAX];
} sh_complete_req_t;

typedef struct {
    char *dir_open;
    size_t dir_open_len;
    char *typed_dir;
    size_t typed_dir_len;
    char *base_prefix;
    size_t base_prefix_len;
} prefix_parts_t;

typedef struct {
    char *buf;
    size_t cap;
    size_t *len;
    size_t *cursor;
} line_edit_t;

static sh_complete_state_t sh_complete = {
    .path = "/bin",
    .color = true,
};

static const char *sh_builtins[] = {
    "bg",   "cd",  "echo", "env",  "exit",  "export", "fg",    "help", "history",
    "jobs", "set", "time", "type", "umask", "unset",  "where", NULL,
};

void complete_set_path(const char *path) {
    if (!path || !path[0]) {
        snprintf(sh_complete.path, sizeof(sh_complete.path), "/bin");
        return;
    }

    snprintf(sh_complete.path, sizeof(sh_complete.path), "%s", path);
}

void complete_set_color(bool enabled) {
    sh_complete.color = enabled;
}

static bool is_word_delim(char ch) {
    switch (ch) {
    case '\0':
    case ' ':
    case '\t':
    case '\n':
    case '|':
    case '&':
    case '<':
    case '>':
    case ';':
        return true;
    default:
        return false;
    }
}

static bool is_command_position(const char *buf, size_t token_start) {
    if (!buf) {
        return false;
    }

    size_t i = token_start;
    while (i > 0 && (buf[i - 1] == ' ' || buf[i - 1] == '\t')) {
        i--;
    }

    if (!i) {
        return true;
    }

    char ch = buf[i - 1];
    return ch == '|' || ch == ';' || ch == '&';
}

static bool starts_with(const char *text, const char *prefix) {
    if (!text || !prefix) {
        return false;
    }

    while (*prefix) {
        if (*text++ != *prefix++) {
            return false;
        }
    }

    return true;
}

static size_t completion_term_cols(void) {
    const term_size_t fallback = {
        .rows = 25,
        .cols = 80,
    };

    term_size_t size = fallback;
    term_get_size(STDIN_FILENO, STDOUT_FILENO, &size, &fallback);
    return size.cols;
}

static int match_name_cmp(const void *lhs, const void *rhs) {
    const sh_match_t *a = (const sh_match_t *)lhs;
    const sh_match_t *b = (const sh_match_t *)rhs;
    return strcmp(a->name, b->name);
}

static size_t match_display_len(const sh_match_t *match) {
    if (!match) {
        return 0;
    }

    return strlen(match->name) + (match->is_dir ? 1 : 0);
}

static size_t lcp_len(const sh_match_t *matches, size_t count) {
    if (!matches || !count) {
        return 0;
    }

    size_t len = strlen(matches[0].name);
    for (size_t i = 1; i < count; i++) {
        size_t j = 0;

        const char *a = matches[0].name;
        const char *b = matches[i].name;

        while (j < len && b[j] && a[j] == b[j]) {
            j++;
        }

        len = j;
    }

    return len;
}

static bool match_is_dir(const char *dir_path, const char *name) {
    if (!dir_path || !name) {
        return false;
    }

    char full[SH_PATH_MAX];
    if (!fs_join_path(full, sizeof(full), dir_path, name)) {
        return false;
    }

    struct stat st;
    if (stat(full, &st) < 0) {
        return false;
    }

    return (st.st_mode & S_IFMT) == S_IFDIR;
}

static bool command_is_runnable(const char *dir_path, const char *name) {
    if (!dir_path || !name) {
        return false;
    }

    char full[SH_PATH_MAX];
    if (!fs_join_path(full, sizeof(full), dir_path, name)) {
        return false;
    }

    struct stat st;
    if (stat(full, &st) < 0) {
        return false;
    }

    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        return false;
    }

    return !access(full, X_OK);
}

static bool add_match(sh_match_t *matches, size_t *count, size_t cap, const char *name, bool is_dir) {
    if (!matches || !count || !name || !name[0]) {
        return false;
    }

    for (size_t i = 0; i < *count; i++) {
        if (strcmp(matches[i].name, name)) {
            continue;
        }

        if (is_dir) {
            matches[i].is_dir = true;
        }

        return true;
    }

    if (*count >= cap) {
        return false;
    }

    sh_match_t *match = &matches[(*count)++];
    snprintf(match->name, sizeof(match->name), "%s", name);
    match->is_dir = is_dir;

    return true;
}

static size_t
collect_matches(const char *dir_path, const char *prefix, bool include_hidden, sh_match_t *matches, size_t cap) {
    if (!dir_path || !prefix || !matches || !cap) {
        return 0;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        return 0;
    }

    size_t count = 0;
    struct dirent *dent = NULL;

    while ((dent = readdir(dir)) != NULL) {
        if (!dent->d_name[0]) {
            continue;
        }

        if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..")) {
            continue;
        }

        if (!include_hidden && dent->d_name[0] == '.') {
            continue;
        }

        if (!starts_with(dent->d_name, prefix)) {
            continue;
        }

        add_match(matches, &count, cap, dent->d_name, match_is_dir(dir_path, dent->d_name));
    }

    closedir(dir);
    return count;
}

static size_t collect_commands(const char *prefix, bool include_hidden, sh_match_t *matches, size_t cap) {
    if (!prefix || !matches || !cap) {
        return 0;
    }

    char path_buf[SH_PATH_MAX];
    snprintf(path_buf, sizeof(path_buf), "%s", sh_complete.path);

    size_t count = 0;

    for (const char **bp = sh_builtins; *bp; bp++) {
        if (!starts_with(*bp, prefix)) {
            continue;
        }

        add_match(matches, &count, cap, *bp, false);
    }

    char *cursor = path_buf;

    while (cursor && *cursor) {
        char *next = strchr(cursor, ':');

        if (next) {
            *next = '\0';
        }

        const char *dir = cursor[0] ? cursor : ".";
        DIR *dirp = opendir(dir);

        if (dirp) {
            struct dirent *dent = NULL;

            while ((dent = readdir(dirp)) != NULL) {
                if (!dent->d_name[0]) {
                    continue;
                }

                if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..")) {
                    continue;
                }

                if (!include_hidden && dent->d_name[0] == '.') {
                    continue;
                }

                if (!starts_with(dent->d_name, prefix)) {
                    continue;
                }

                if (!command_is_runnable(dir, dent->d_name)) {
                    continue;
                }

                add_match(matches, &count, cap, dent->d_name, false);
            }

            closedir(dirp);
        }

        if (!next) {
            break;
        }

        cursor = next + 1;
    }

    return count;
}

static void list_matches(sh_match_t *matches, size_t count) {
    if (!matches || !count) {
        return;
    }

    qsort(matches, count, sizeof(matches[0]), match_name_cmp);

    size_t max_len = 0;

    for (size_t i = 0; i < count; i++) {
        size_t width = match_display_len(&matches[i]);

        if (width > max_len) {
            max_len = width;
        }
    }

    size_t cols = completion_term_cols();

    size_t cell = max_len + 2;
    if (!cell) {
        cell = 1;
    }

    size_t per_row = cols / cell;
    if (!per_row) {
        per_row = 1;
    }

    io_write_str("\n");

    for (size_t i = 0; i < count; i++) {
        char item[NAME_MAX + 2];
        snprintf(item, sizeof(item), "%s%s", matches[i].name, matches[i].is_dir ? "/" : "");

        if (sh_complete.color && matches[i].is_dir) {
            io_write_str(SH_C_BLUE);
        }

        io_write_str(item);

        if (sh_complete.color && matches[i].is_dir) {
            io_write_str(SH_C_RESET);
        }

        bool end_row = ((i + 1) % per_row) == 0 || i + 1 == count;
        if (end_row) {
            io_write_str("\n");
            continue;
        }

        size_t pad = cell - strlen(item);
        while (pad-- > 0) {
            io_write_str(" ");
        }
    }
}

static bool build_candidate(char *out, size_t out_len, const char *typed_dir, const char *name, bool is_dir) {
    if (!out || !out_len || !typed_dir || !name) {
        return false;
    }

    int length;
    if (is_dir) {
        length = snprintf(out, out_len, "%s%s/", typed_dir, name);
    } else {
        length = snprintf(out, out_len, "%s%s", typed_dir, name);
    }

    return length >= 0 && (size_t)length < out_len;
}

static void split_prefix(const char *token, size_t token_len, prefix_parts_t *parts) {
    if (!token || !parts || !parts->dir_open || !parts->typed_dir || !parts->base_prefix) {
        return;
    }

    ssize_t slash = -1;

    for (size_t i = 0; i < token_len; i++) {
        if (token[i] == '/') {
            slash = (ssize_t)i;
        }
    }

    if (slash < 0) {
        snprintf(parts->dir_open, parts->dir_open_len, ".");

        if (parts->typed_dir_len) {
            parts->typed_dir[0] = '\0';
        }

        snprintf(parts->base_prefix, parts->base_prefix_len, "%.*s", (int)token_len, token);
        return;
    }

    size_t typed_len = (size_t)slash + 1;

    snprintf(parts->typed_dir, parts->typed_dir_len, "%.*s", (int)typed_len, token);
    snprintf(parts->base_prefix, parts->base_prefix_len, "%.*s", (int)(token_len - typed_len), token + typed_len);

    if (!slash) {
        snprintf(parts->dir_open, parts->dir_open_len, "/");
    } else {
        snprintf(parts->dir_open, parts->dir_open_len, "%.*s", (int)slash, token);
    }
}

static bool read_complete_req(const char *buf, size_t cursor, sh_complete_req_t *req) {
    size_t token_start = cursor;
    while (token_start > 0 && !is_word_delim(buf[token_start - 1])) {
        token_start--;
    }

    if (cursor == token_start) {
        return false;
    }

    size_t token_len = cursor - token_start;

    if (token_len + 1 > sizeof(req->token)) {
        return false;
    }

    memset(req, 0, sizeof(*req));
    req->token_start = token_start;
    req->token_len = token_len;

    memcpy(req->token, buf + token_start, token_len);
    req->token[token_len] = '\0';

    req->command = !strchr(req->token, '/') && is_command_position(buf, token_start);
    if (!req->command) {
        prefix_parts_t parts = {
            .dir_open = req->dir_open,
            .dir_open_len = sizeof(req->dir_open),
            .typed_dir = req->typed_dir,
            .typed_dir_len = sizeof(req->typed_dir),
            .base_prefix = req->base_prefix,
            .base_prefix_len = sizeof(req->base_prefix),
        };

        split_prefix(req->token, req->token_len, &parts);
    }

    return true;
}

static size_t collect_req_matches(const sh_complete_req_t *req, sh_match_t *matches) {
    if (req->command) {
        bool include_hidden = req->token[0] == '.';
        return collect_commands(req->token, include_hidden, matches, SH_MATCH_MAX);
    }

    bool include_hidden = req->base_prefix[0] == '.';
    return collect_matches(req->dir_open, req->base_prefix, include_hidden, matches, SH_MATCH_MAX);
}

static bool command_completion(char *out, size_t out_len, const char *name) {
    int length = snprintf(out, out_len, "%s", name);
    return length >= 0 && (size_t)length < out_len;
}

static bool build_completion(
    const sh_complete_req_t *req,
    sh_match_t *matches,
    size_t match_count,
    char *completion,
    sh_complete_result_t *result
) {
    if (match_count == 1) {
        if (req->command) {
            return command_completion(completion, SH_PATH_MAX, matches[0].name);
        }

        return build_candidate(completion, SH_PATH_MAX, req->typed_dir, matches[0].name, matches[0].is_dir);
    }

    size_t common = lcp_len(matches, match_count);
    size_t base_len = req->command ? req->token_len : strlen(req->base_prefix);

    if (common <= base_len) {
        list_matches(matches, match_count);

        if (result) {
            result->listed = true;
        }

        return false;
    }

    char common_name[NAME_MAX + 1];
    snprintf(common_name, sizeof(common_name), "%.*s", (int)common, matches[0].name);

    if (req->command) {
        return command_completion(completion, SH_PATH_MAX, common_name);
    }

    return build_candidate(completion, SH_PATH_MAX, req->typed_dir, common_name, false);
}

static bool apply_completion(
    line_edit_t *line,
    const sh_complete_req_t *req,
    const char *completion,
    sh_complete_result_t *result
) {
    if (!line || !line->buf || !line->len || !line->cursor || !req || !completion) {
        return false;
    }

    size_t completion_len = strlen(completion);
    size_t tail_len = *line->len - *line->cursor;
    size_t replaced_len = *line->cursor - req->token_start;
    size_t new_len = *line->len - replaced_len + completion_len;

    if (new_len + 1 > line->cap) {
        return false;
    }

    memmove(line->buf + req->token_start + completion_len, line->buf + *line->cursor, tail_len + 1);
    memcpy(line->buf + req->token_start, completion, completion_len);

    size_t old_cursor = *line->cursor;
    *line->len = new_len;
    *line->cursor = req->token_start + completion_len;

    if (!result) {
        return true;
    }

    result->changed = true;

    if (completion_len > req->token_len && !strncmp(completion, req->token, req->token_len)) {
        result->erase_valid = true;
        result->erase_start = old_cursor;
        result->erase_end = old_cursor + (completion_len - req->token_len);
    }

    return true;
}

void complete_line(char *buf, size_t cap, size_t *len, size_t *cursor, sh_complete_result_t *result) {
    if (!buf || !cap || !len || !cursor || *cursor > *len) {
        return;
    }

    if (result) {
        memset(result, 0, sizeof(*result));
    }

    sh_complete_req_t req = { 0 };
    if (!read_complete_req(buf, *cursor, &req)) {
        return;
    }

    sh_match_t *matches = malloc(SH_MATCH_MAX * sizeof(sh_match_t));
    if (!matches) {
        return;
    }

    size_t match_count = collect_req_matches(&req, matches);
    if (!match_count) {
        free(matches);
        return;
    }

    char completion[SH_PATH_MAX];
    bool have_completion = build_completion(&req, matches, match_count, completion, result);

    if (have_completion) {
        line_edit_t line = {
            .buf = buf,
            .cap = cap,
            .len = len,
            .cursor = cursor,
        };

        (void)apply_completion(&line, &req, completion, result);
    }

    free(matches);
}
