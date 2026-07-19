#include "cfg.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static char *trim_left(char *text) {
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }

    return text;
}

static void trim_right(char *text) {
    size_t len = strlen(text);

    while (len && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

static void strip_comment(char *text) {
    char quote = '\0';

    for (char *pos = text; *pos; pos++) {
        if (quote) {
            if (*pos == '\\' && pos[1]) {
                pos++;
            } else if (*pos == quote) {
                quote = '\0';
            }

            continue;
        }

        if (*pos == '\'' || *pos == '"') {
            quote = *pos;
        } else if (*pos == '#') {
            *pos = '\0';
            return;
        }
    }
}

static char *unquote(char *text) {
    size_t len = strlen(text);
    if (len < 2 || (text[0] != '\'' && text[0] != '"') || text[len - 1] != text[0]) {
        return text;
    }

    text[len - 1] = '\0';
    return text + 1;
}

void parse_cfg(char *text, const cfg_entry_t *table, void *data) {
    if (!text || !table) {
        return;
    }

    char *tok_pos = NULL;
    char *line = strtok_r(text, "\n", &tok_pos);

    while (line) {
        char *pos = trim_left(line);

        if (!isalnum((unsigned char)*pos)) {
            goto next;
        }

        char *key = pos;
        while (*pos && !isspace((unsigned char)*pos) && *pos != '=') {
            pos++;
        }

        if (!*pos) {
            goto next;
        }

        *pos++ = '\0';
        pos = trim_left(pos);

        if (*pos == '=') {
            pos = trim_left(pos + 1);
        }

        strip_comment(pos);
        trim_right(pos);

        if (!*pos) {
            goto next;
        }

        char *value = unquote(pos);

        for (const cfg_entry_t *e = table; e->key; e++) {
            if (!strcasecmp(e->key, key)) {
                e->handler(value, data);
                break;
            }
        }

    next:
        line = strtok_r(NULL, "\n", &tok_pos);
    }
}
