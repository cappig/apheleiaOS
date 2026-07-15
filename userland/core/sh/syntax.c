#include "syntax.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char *syntax_trim(char *text) {
    while (text && *text && isspace((unsigned char)*text)) {
        text++;
    }

    if (!text) {
        return NULL;
    }

    size_t len = strlen(text);
    while (len && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }

    return text;
}

bool syntax_starts(const char *text, const char *word) {
    size_t len = strlen(word);
    return text && !strncmp(text, word, len) && (!text[len] || isspace((unsigned char)text[len]));
}

bool syntax_name(const char *name, size_t len) {
    if (!name || !len || !(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        return false;
    }

    for (size_t i = 1; i < len; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return false;
        }
    }

    return true;
}

size_t syntax_sub_len(const char *text) {
    if (!text || text[0] != '$' || text[1] != '(') {
        return 0;
    }

    bool single = false;
    bool double_quote = false;
    bool escape = false;

    for (size_t i = 2; text[i]; i++) {
        char ch = text[i];

        if (escape) {
            escape = false;
            continue;
        }
        if (!single && ch == '\\') {
            escape = true;
            continue;
        }
        if (!double_quote && ch == '\'') {
            single = !single;
            continue;
        }
        if (!single && ch == '"') {
            double_quote = !double_quote;
            continue;
        }
        if (!single && ch == '$' && text[i + 1] == '(') {
            size_t nested = syntax_sub_len(text + i);
            if (!nested) {
                return 0;
            }
            i += nested - 1;
            continue;
        }
        if (!single && !double_quote && ch == ')') {
            return i + 1;
        }
    }

    return 0;
}

static bool add_stmt(script_list_t *list, const char *start, size_t len, syntax_error_fn error) {
    while (len && isspace((unsigned char)*start)) {
        start++;
        len--;
    }
    while (len && isspace((unsigned char)start[len - 1])) {
        len--;
    }

    if (!len) {
        return true;
    }
    if (list->count >= SCRIPT_MAX_STMTS) {
        error("too many script statements");
        return false;
    }

    char *text = malloc(len + 1);
    if (!text) {
        error("out of memory");
        return false;
    }
    memcpy(text, start, len);
    text[len] = '\0';

    if (syntax_starts(text, "case")) {
        bool single = false;
        bool double_quote = false;

        for (size_t i = 4; i + 3 < len; i++) {
            if (text[i] == '$' && text[i + 1] == '(') {
                size_t span = syntax_sub_len(text + i);
                if (span) {
                    i += span - 1;
                    continue;
                }
            }
            if (text[i] == '\'' && !double_quote) {
                single = !single;
            } else if (text[i] == '"' && !single) {
                double_quote = !double_quote;
            }

            bool in_word = isspace((unsigned char)text[i]) && text[i + 1] == 'i' && text[i + 2] == 'n' &&
                           isspace((unsigned char)text[i + 3]);
            if (!single && !double_quote && in_word) {
                bool ok = add_stmt(list, text, i + 3, error);
                ok = ok && add_stmt(list, text + i + 3, len - i - 3, error);
                free(text);
                return ok;
            }
        }
    }

    static const char *prefixes[] = { "then", "do", "else", NULL };
    for (const char **word = prefixes; *word; word++) {
        size_t word_len = strlen(*word);
        if (len > word_len && !strncmp(text, *word, word_len) && isspace((unsigned char)text[word_len])) {
            bool ok = add_stmt(list, text, word_len, error);
            ok = ok && add_stmt(list, text + word_len, len - word_len, error);
            free(text);
            return ok;
        }
    }

    list->items[list->count].text = text;
    list->count++;
    return true;
}

void syntax_free(script_list_t *list) {
    if (!list) {
        return;
    }

    for (int i = 0; i < list->count; i++) {
        free(list->items[i].text);
    }
    memset(list, 0, sizeof(*list));
}

bool syntax_split(const char *source, script_list_t *list, syntax_error_fn error) {
    if (!source || !list || !error) {
        return false;
    }

    memset(list, 0, sizeof(*list));

    const char *start = source;
    bool single = false;
    bool double_quote = false;
    bool escape = false;
    for (const char *cursor = source; *cursor; cursor++) {
        char ch = *cursor;

        if (escape) {
            escape = false;
            continue;
        }
        if (!single && ch == '\\') {
            escape = true;
            continue;
        }
        if (!double_quote && ch == '\'') {
            single = !single;
            continue;
        }
        if (!single && ch == '"') {
            double_quote = !double_quote;
            continue;
        }
        if (!single && ch == '$' && cursor[1] == '(') {
            size_t len = syntax_sub_len(cursor);
            if (!len) {
                list->incomplete = true;
                return true;
            }
            cursor += len - 1;
            continue;
        }
        if (single || double_quote) {
            continue;
        }

        bool at_word_start = cursor == start || isspace((unsigned char)cursor[-1]);
        if (ch == '#' && at_word_start) {
            if (!add_stmt(list, start, (size_t)(cursor - start), error)) {
                return false;
            }
            while (cursor[1] && cursor[1] != '\n') {
                cursor++;
            }
            start = cursor + 1;
            continue;
        }

        bool brace_left = cursor == source || isspace((unsigned char)cursor[-1]) || (ch == '{' && cursor[-1] == ')') ||
                          (ch == '}' && cursor[-1] == ';');
        bool brace_right = !cursor[1] || isspace((unsigned char)cursor[1]) || cursor[1] == ';';
        bool brace = (ch == '{' || ch == '}') && brace_left && brace_right;
        if (ch != ';' && ch != '\n' && !brace) {
            continue;
        }

        if (!add_stmt(list, start, (size_t)(cursor - start), error)) {
            return false;
        }
        if (brace && !add_stmt(list, cursor, 1, error)) {
            return false;
        }
        if (ch == ';' && cursor[1] == ';') {
            if (!add_stmt(list, ";;", 2, error)) {
                return false;
            }
            cursor++;
        }
        start = cursor + 1;
    }

    if (single || double_quote || escape) {
        list->incomplete = true;
        return true;
    }

    return add_stmt(list, start, strlen(start), error);
}

stmt_kind_t syntax_kind(const char *text) {
    if (!text) {
        return STMT_SIMPLE;
    }
    if (syntax_starts(text, "if")) {
        return STMT_IF;
    }
    if (!strcmp(text, "then")) {
        return STMT_THEN;
    }
    if (syntax_starts(text, "elif")) {
        return STMT_ELIF;
    }
    if (!strcmp(text, "else")) {
        return STMT_ELSE;
    }
    if (!strcmp(text, "fi")) {
        return STMT_FI;
    }
    if (syntax_starts(text, "while")) {
        return STMT_WHILE;
    }
    if (syntax_starts(text, "for")) {
        return STMT_FOR;
    }
    if (!strcmp(text, "do")) {
        return STMT_DO;
    }
    if (!strcmp(text, "done")) {
        return STMT_DONE;
    }
    if (syntax_starts(text, "case")) {
        return STMT_CASE;
    }
    if (!strcmp(text, "esac")) {
        return STMT_ESAC;
    }
    if (!strcmp(text, "{")) {
        return STMT_OPEN_BRACE;
    }
    if (!strcmp(text, "}")) {
        return STMT_CLOSE_BRACE;
    }
    if (!strcmp(text, ";;")) {
        return STMT_CASE_END;
    }
    return STMT_SIMPLE;
}

int syntax_nest(stmt_kind_t kind) {
    switch (kind) {
    case STMT_IF:
    case STMT_WHILE:
    case STMT_FOR:
    case STMT_CASE:
    case STMT_OPEN_BRACE:
        return 1;
    case STMT_FI:
    case STMT_DONE:
    case STMT_ESAC:
    case STMT_CLOSE_BRACE:
        return -1;
    default:
        return 0;
    }
}

int syntax_find(script_list_t *list, int start, int end, unsigned int wanted) {
    int depth = 0;

    for (int i = start; i < end; i++) {
        stmt_kind_t kind = syntax_kind(list->items[i].text);
        int change = syntax_nest(kind);

        if (change < 0 && depth > 0) {
            depth--;
            continue;
        }
        if (!depth && (wanted & (1U << kind))) {
            return i;
        }
        if (change > 0) {
            depth++;
        }
    }

    return -1;
}

int syntax_words(char *text, char **words, int max_words) {
    int count = 0;
    char *src = text;

    while (src && *src) {
        while (isspace((unsigned char)*src)) {
            src++;
        }
        if (!*src) {
            break;
        }
        if (count >= max_words) {
            return -1;
        }

        words[count++] = src;
        bool single = false;
        bool double_quote = false;
        bool escape = false;

        while (*src) {
            char ch = *src;
            if (escape) {
                escape = false;
                src++;
                continue;
            }
            if (!single && ch == '\\') {
                escape = true;
                src++;
                continue;
            }
            if (!single && ch == '$' && src[1] == '(') {
                size_t span = syntax_sub_len(src);
                if (!span) {
                    return -1;
                }
                src += span;
                continue;
            }
            if (!double_quote && ch == '\'') {
                single = !single;
            } else if (!single && ch == '"') {
                double_quote = !double_quote;
            } else if (!single && !double_quote && isspace((unsigned char)ch)) {
                *src++ = '\0';
                break;
            }
            src++;
        }
    }

    return count;
}

char *syntax_case_end(char *text) {
    bool single = false;
    bool double_quote = false;
    bool escape = false;

    for (char *p = text; *p; p++) {
        if (escape) {
            escape = false;
            continue;
        }
        if (!single && *p == '\\') {
            escape = true;
            continue;
        }
        if (!single && *p == '$' && p[1] == '(') {
            size_t span = syntax_sub_len(p);
            if (!span) {
                return NULL;
            }
            p += span - 1;
            continue;
        }
        if (!double_quote && *p == '\'') {
            single = !single;
            continue;
        }
        if (!single && *p == '"') {
            double_quote = !double_quote;
            continue;
        }
        if (!single && !double_quote && *p == ')') {
            return p;
        }
    }

    return NULL;
}
