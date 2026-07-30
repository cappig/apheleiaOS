#include "script.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "syntax.h"

#define SCRIPT_MAX_FUNCS 16
#define SCRIPT_MAX_CALLS 32
#define SCRIPT_NAME_MAX  32
#define SCRIPT_TEXT_MAX  1024
#define SCRIPT_WORD_MAX  256

typedef enum {
    FLOW_NONE,
    FLOW_BREAK,
    FLOW_CONTINUE,
    FLOW_RETURN,
} flow_t;

typedef struct {
    int status;
    flow_t flow;
} script_result_t;

typedef struct {
    char name[SCRIPT_NAME_MAX];
    char *body;
} script_func_t;

static sh_script_ops_t script_ops;
static script_func_t functions[SCRIPT_MAX_FUNCS];
static int function_count;
static int call_count;

static script_result_t run_range(script_list_t *list, int start, int end, int loop_depth, bool in_function);

static script_result_t result_make(int status, flow_t flow) {
    script_result_t result = {
        .status = status,
        .flow = flow,
    };

    return result;
}

static void report_error(const char *message) {
    if (script_ops.report_error) {
        script_ops.report_error(message);
    }
}

static void report_missing(const char *command, const char *part) {
    char message[64];
    snprintf(message, sizeof(message), "%s: missing %s", command, part);
    report_error(message);
}

static script_result_t run_command(const char *command) {
    if (!command || !command[0]) {
        return result_make(0, FLOW_NONE);
    }

    if (!script_ops.run_command) {
        return result_make(1, FLOW_NONE);
    }

    return result_make(script_ops.run_command(command), FLOW_NONE);
}

static script_result_t
run_condition(script_list_t *list, const char *first, int start, int end, int loop_depth, bool in_function) {
    script_result_t result = run_command(first);
    if (result.flow != FLOW_NONE) {
        return result;
    }

    if (start < end) {
        result = run_range(list, start, end, loop_depth, in_function);
    }

    return result;
}

typedef struct {
    int body;
    int stop;
    bool matched;
} if_arm_t;

static script_result_t eval_if_arm(
    script_list_t *list,
    int index,
    int end,
    size_t keyword_len,
    int loop_depth,
    bool in_function,
    if_arm_t *arm
) {
    arm->stop = -1;
    arm->matched = false;
    const char *keyword = keyword_len == 2 ? "if" : "elif";

    unsigned int stops = (1U << STMT_ELIF) | (1U << STMT_ELSE) | (1U << STMT_FI);
    int then_at = syntax_find(list, index + 1, end, 1U << STMT_THEN);
    if (then_at < 0) {
        report_missing(keyword, "then");
        return result_make(2, FLOW_NONE);
    }

    arm->body = then_at + 1;
    arm->stop = syntax_find(list, arm->body, end, stops);
    if (arm->stop < 0) {
        report_error("if: missing fi");
        return result_make(2, FLOW_NONE);
    }

    const char *condition = syntax_trim(list->items[index].text + keyword_len);
    if (!condition[0] && index + 1 == then_at) {
        report_missing(keyword, "condition");
        arm->stop = -1;
        return result_make(2, FLOW_NONE);
    }

    script_result_t result = run_condition(list, condition, index + 1, then_at, loop_depth, in_function);
    arm->matched = result.flow == FLOW_NONE && result.status == 0;
    return result;
}

static script_result_t
run_if_body(script_list_t *list, int start, int stop, int end, int loop_depth, bool in_function, int *next) {
    int fi_at = syntax_find(list, stop, end, 1U << STMT_FI);
    if (fi_at < 0) {
        report_error("if: missing fi");
        return result_make(2, FLOW_NONE);
    }

    script_result_t result = run_range(list, start, stop, loop_depth, in_function);
    *next = fi_at + 1;
    return result;
}

static script_result_t run_if(script_list_t *list, int index, int end, int loop_depth, bool in_function, int *next) {
    if_arm_t arm = { 0 };
    script_result_t result = eval_if_arm(list, index, end, 2, loop_depth, in_function, &arm);
    if (result.flow != FLOW_NONE || arm.stop < 0) {
        return result;
    }
    if (arm.matched) {
        return run_if_body(list, arm.body, arm.stop, end, loop_depth, in_function, next);
    }

    while (syntax_kind(list->items[arm.stop].text) == STMT_ELIF) {
        int elif_at = arm.stop;
        result = eval_if_arm(list, elif_at, end, 4, loop_depth, in_function, &arm);
        if (result.flow != FLOW_NONE || arm.stop < 0) {
            return result;
        }
        if (arm.matched) {
            return run_if_body(list, arm.body, arm.stop, end, loop_depth, in_function, next);
        }
    }

    stmt_kind_t kind = syntax_kind(list->items[arm.stop].text);
    if (kind == STMT_ELSE) {
        int fi_at = syntax_find(list, arm.stop + 1, end, 1U << STMT_FI);
        if (fi_at < 0) {
            report_error("if: missing fi");
            return result_make(2, FLOW_NONE);
        }
        script_result_t else_result = run_range(list, arm.stop + 1, fi_at, loop_depth, in_function);
        *next = fi_at + 1;
        return else_result;
    }
    if (kind != STMT_FI) {
        report_error("if: invalid branch");
        return result_make(2, FLOW_NONE);
    }

    *next = arm.stop + 1;
    return result_make(0, FLOW_NONE);
}

static script_result_t run_while(script_list_t *list, int index, int end, int loop_depth, bool in_function, int *next) {
    int do_at = syntax_find(list, index + 1, end, 1U << STMT_DO);
    if (do_at < 0) {
        report_error("while: missing do");
        return result_make(2, FLOW_NONE);
    }

    int done_at = syntax_find(list, do_at + 1, end, 1U << STMT_DONE);
    if (done_at < 0) {
        report_error("while: missing done");
        return result_make(2, FLOW_NONE);
    }

    const char *condition = syntax_trim(list->items[index].text + 5);
    if (!condition[0] && index + 1 == do_at) {
        report_error("while: missing condition");
        return result_make(2, FLOW_NONE);
    }
    script_result_t result = result_make(0, FLOW_NONE);

    for (;;) {
        script_result_t test = run_condition(list, condition, index + 1, do_at, loop_depth + 1, in_function);
        if (test.flow != FLOW_NONE) {
            return test;
        }
        if (test.status) {
            break;
        }

        result = run_range(list, do_at + 1, done_at, loop_depth + 1, in_function);
        if (result.flow == FLOW_BREAK) {
            result = result_make(0, FLOW_NONE);
            break;
        }
        if (result.flow == FLOW_CONTINUE) {
            result = result_make(0, FLOW_NONE);
            continue;
        }
        if (result.flow == FLOW_RETURN) {
            return result;
        }
    }

    *next = done_at + 1;
    return result;
}

static script_result_t run_for(script_list_t *list, int index, int end, int loop_depth, bool in_function, int *next) {
    int do_at = syntax_find(list, index + 1, end, 1U << STMT_DO);
    int done_at = do_at < 0 ? -1 : syntax_find(list, do_at + 1, end, 1U << STMT_DONE);
    if (do_at < 0 || done_at < 0) {
        report_error(do_at < 0 ? "for: missing do" : "for: missing done");
        return result_make(2, FLOW_NONE);
    }

    char header[SCRIPT_TEXT_MAX];
    if (strlen(list->items[index].text + 3) >= sizeof(header)) {
        report_error("for: header is too long");
        return result_make(2, FLOW_NONE);
    }
    snprintf(header, sizeof(header), "%s", syntax_trim(list->items[index].text + 3));

    char *words[SCRIPT_MAX_WORDS];
    int word_count = syntax_words(header, words, SCRIPT_MAX_WORDS);
    if (word_count < 1 || !syntax_name(words[0], strlen(words[0]))) {
        report_error("for: invalid variable name");
        return result_make(2, FLOW_NONE);
    }

    int first_value = 1;
    bool use_args = true;
    if (word_count > 1) {
        if (strcmp(words[1], "in")) {
            report_error("for: expected in");
            return result_make(2, FLOW_NONE);
        }
        first_value = 2;
        use_args = false;
    }

    int value_count = use_args && script_ops.arg_count ? script_ops.arg_count() : word_count - first_value;
    script_result_t result = result_make(0, FLOW_NONE);

    for (int i = 0; i < value_count; i++) {
        char value[SCRIPT_WORD_MAX];
        const char *raw = use_args && script_ops.arg_at ? script_ops.arg_at(i + 1) : words[first_value + i];

        if (!raw) {
            raw = "";
        }

        if (use_args) {
            snprintf(value, sizeof(value), "%s", raw);
        } else if (!script_ops.expand_word || !script_ops.expand_word(raw, value, sizeof(value))) {
            report_error("for: expansion failed");
            return result_make(1, FLOW_NONE);
        }

        if (!script_ops.set_var || !script_ops.set_var(words[0], value)) {
            report_error("for: could not set variable");
            return result_make(1, FLOW_NONE);
        }

        result = run_range(list, do_at + 1, done_at, loop_depth + 1, in_function);
        if (result.flow == FLOW_BREAK) {
            result = result_make(0, FLOW_NONE);
            break;
        }
        if (result.flow == FLOW_CONTINUE) {
            result = result_make(0, FLOW_NONE);
            continue;
        }
        if (result.flow == FLOW_RETURN) {
            return result;
        }
    }

    *next = done_at + 1;
    return result;
}

static bool glob_match(const char *pattern, const char *text) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) {
                return true;
            }
            while (*text) {
                if (glob_match(pattern, text++)) {
                    return true;
                }
            }
            return false;
        }
        if (*pattern == '?') {
            if (!*text) {
                return false;
            }
        } else if (*pattern != *text) {
            return false;
        }
        pattern++;
        text++;
    }

    return !*text;
}

static bool arm_matches(char *patterns, const char *value) {
    char *start = patterns;
    bool single = false;
    bool double_quote = false;

    for (char *p = patterns;; p++) {
        if (*p == '$' && p[1] == '(' && !single) {
            size_t span = syntax_sub_len(p);
            if (!span) {
                return false;
            }
            p += span - 1;
            continue;
        }
        if (*p == '\'' && !double_quote) {
            single = !single;
        } else if (*p == '"' && !single) {
            double_quote = !double_quote;
        }

        if ((*p == '|' && !single && !double_quote) || !*p) {
            char saved = *p;
            *p = '\0';

            char expanded[SCRIPT_WORD_MAX];
            bool ok = script_ops.expand_word && script_ops.expand_word(syntax_trim(start), expanded, sizeof(expanded));
            *p = saved;

            if (ok && glob_match(expanded, value)) {
                return true;
            }
            if (!saved) {
                return false;
            }
            start = p + 1;
        }
    }
}

static script_result_t run_case(script_list_t *list, int index, int end, int loop_depth, bool in_function, int *next) {
    int esac_at = syntax_find(list, index + 1, end, 1U << STMT_ESAC);
    if (esac_at < 0) {
        report_error("case: missing esac");
        return result_make(2, FLOW_NONE);
    }

    char header[SCRIPT_TEXT_MAX];
    if (strlen(list->items[index].text + 4) >= sizeof(header)) {
        report_error("case: header is too long");
        return result_make(2, FLOW_NONE);
    }
    snprintf(header, sizeof(header), "%s", syntax_trim(list->items[index].text + 4));
    char *words[3];
    int word_count = syntax_words(header, words, 3);
    int arm_at = index + 1;

    if (word_count == 2 && !strcmp(words[1], "in")) {
        /* The common `case WORD in` form. */
    } else if (word_count == 1 && arm_at < esac_at && !strcmp(list->items[arm_at].text, "in")) {
        arm_at++;
    } else {
        report_error("case: expected WORD in");
        return result_make(2, FLOW_NONE);
    }

    char value[SCRIPT_WORD_MAX];
    if (!script_ops.expand_word || !script_ops.expand_word(words[0], value, sizeof(value))) {
        report_error("case: expansion failed");
        return result_make(1, FLOW_NONE);
    }

    script_result_t result = result_make(0, FLOW_NONE);
    while (arm_at < esac_at) {
        char *close = syntax_case_end(list->items[arm_at].text);
        if (!close) {
            report_error("case: expected pattern)");
            return result_make(2, FLOW_NONE);
        }

        *close = '\0';
        char *tail = syntax_trim(close + 1);
        int stop = syntax_find(list, arm_at + 1, esac_at, 1U << STMT_CASE_END);
        if (stop < 0) {
            stop = esac_at;
        }

        bool matched = arm_matches(list->items[arm_at].text, value);
        *close = ')';

        if (matched) {
            int body_start = arm_at + 1;
            if (tail && tail[0]) {
                char *pattern = list->items[arm_at].text;
                list->items[arm_at].text = tail;
                body_start = arm_at;
                result = run_range(list, body_start, stop, loop_depth, in_function);
                list->items[arm_at].text = pattern;
            } else if (body_start < stop) {
                result = run_range(list, body_start, stop, loop_depth, in_function);
            }
            *next = esac_at + 1;
            return result;
        }

        arm_at = stop + 1;
    }

    *next = esac_at + 1;
    return result;
}

static bool function_name(const char *text, char *name, size_t name_len) {
    if (!text || !name || name_len < 2) {
        return false;
    }

    const char *open = strchr(text, '(');
    if (!open) {
        return false;
    }

    const char *close = strchr(open + 1, ')');
    if (!close || close[1] || close != open + 1) {
        return false;
    }

    size_t len = (size_t)(open - text);
    while (len && isspace((unsigned char)text[len - 1])) {
        len--;
    }

    if (!syntax_name(text, len) || len >= name_len) {
        return false;
    }

    memcpy(name, text, len);
    name[len] = '\0';
    return true;
}

static bool save_function(const char *name, script_list_t *list, int start, int end) {
    size_t len = 1;
    for (int i = start; i < end; i++) {
        len += strlen(list->items[i].text) + 2;
    }

    char *body = malloc(len);
    if (!body) {
        report_error("out of memory");
        return false;
    }

    body[0] = '\0';
    for (int i = start; i < end; i++) {
        strcat(body, list->items[i].text);
        strcat(body, !strcmp(list->items[i].text, ";;") ? "\n" : ";\n");
    }

    int slot = -1;
    for (int i = 0; i < function_count; i++) {
        if (!strcmp(functions[i].name, name)) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        if (function_count >= SCRIPT_MAX_FUNCS) {
            free(body);
            report_error("too many shell functions");
            return false;
        }
        slot = function_count++;
        snprintf(functions[slot].name, sizeof(functions[slot].name), "%s", name);
    }

    free(functions[slot].body);
    functions[slot].body = body;
    return true;
}

static int parse_flow_status(const char *text, const char *word, int fallback, bool *ok) {
    const char *arg = syntax_trim((char *)text + strlen(word));
    if (!arg || !arg[0]) {
        *ok = true;
        return fallback;
    }

    char *end = NULL;
    long value = strtol(arg, &end, 10);
    *ok = end && !*end;
    return (int)(value & 0xff);
}

static script_result_t run_range(script_list_t *list, int start, int end, int loop_depth, bool in_function) {
    script_result_t result = result_make(0, FLOW_NONE);

    for (int i = start; i < end;) {
        char *text = list->items[i].text;
        stmt_kind_t kind = syntax_kind(text);
        int next = i + 1;

        if (kind == STMT_IF) {
            result = run_if(list, i, end, loop_depth, in_function, &next);
        } else if (kind == STMT_WHILE) {
            result = run_while(list, i, end, loop_depth, in_function, &next);
        } else if (kind == STMT_FOR) {
            result = run_for(list, i, end, loop_depth, in_function, &next);
        } else if (kind == STMT_CASE) {
            result = run_case(list, i, end, loop_depth, in_function, &next);
        } else {
            char name[SCRIPT_NAME_MAX];
            if (function_name(text, name, sizeof(name))) {
                if (i + 1 >= end || syntax_kind(list->items[i + 1].text) != STMT_OPEN_BRACE) {
                    report_error("function: expected {");
                    return result_make(2, FLOW_NONE);
                }

                int close = syntax_find(list, i + 2, end, 1U << STMT_CLOSE_BRACE);
                if (close < 0) {
                    report_error("function: missing }");
                    return result_make(2, FLOW_NONE);
                }
                if (!save_function(name, list, i + 2, close)) {
                    return result_make(1, FLOW_NONE);
                }
                next = close + 1;
            } else if (syntax_starts(text, "break")) {
                if (!loop_depth) {
                    report_error("break: not in a loop");
                    return result_make(2, FLOW_NONE);
                }
                return result_make(0, FLOW_BREAK);
            } else if (syntax_starts(text, "continue")) {
                if (!loop_depth) {
                    report_error("continue: not in a loop");
                    return result_make(2, FLOW_NONE);
                }
                return result_make(0, FLOW_CONTINUE);
            } else if (syntax_starts(text, "return")) {
                if (!in_function) {
                    report_error("return: not in a function");
                    return result_make(2, FLOW_NONE);
                }
                bool ok = false;
                int status = parse_flow_status(text, "return", result.status, &ok);
                if (!ok) {
                    report_error("return: numeric argument required");
                    return result_make(2, FLOW_NONE);
                }
                return result_make(status, FLOW_RETURN);
            } else if (kind != STMT_SIMPLE) {
                report_error("unexpected shell keyword");
                return result_make(2, FLOW_NONE);
            } else {
                result = run_command(text);
            }
        }

        if (kind != STMT_SIMPLE && next == i + 1) {
            return result;
        }

        if (result.flow != FLOW_NONE) {
            return result;
        }

        i = next;
    }

    return result;
}

static script_result_t run_source(const char *source, bool in_function) {
    script_list_t list;
    if (!syntax_split(source, &list, report_error)) {
        syntax_free(&list);
        return result_make(1, FLOW_NONE);
    }

    if (list.incomplete) {
        report_error("incomplete script");
        syntax_free(&list);
        return result_make(2, FLOW_NONE);
    }

    script_result_t result = run_range(&list, 0, list.count, 0, in_function);
    syntax_free(&list);
    return result;
}

void script_init(const sh_script_ops_t *ops) {
    if (ops) {
        script_ops = *ops;
    } else {
        memset(&script_ops, 0, sizeof(script_ops));
    }
}

int script_run(const char *source) {
    script_result_t result = run_source(source, false);
    return result.status;
}

bool script_has_function(const char *name) {
    if (!name) {
        return false;
    }

    for (int i = 0; i < function_count; i++) {
        if (!strcmp(functions[i].name, name)) {
            return true;
        }
    }

    return false;
}

int script_call(const char *name) {
    for (int i = 0; i < function_count; i++) {
        if (strcmp(functions[i].name, name)) {
            continue;
        }

        if (call_count >= SCRIPT_MAX_CALLS) {
            report_error("function call limit reached");
            return 1;
        }

        call_count++;
        script_result_t result = run_source(functions[i].body, true);
        call_count--;
        return result.status;
    }

    return 127;
}

bool script_needs_more(const char *source) {
    script_list_t list;
    if (!syntax_split(source, &list, report_error)) {
        syntax_free(&list);
        return false;
    }

    if (list.incomplete) {
        syntax_free(&list);
        return true;
    }

    int depth = 0;
    for (int i = 0; i < list.count; i++) {
        depth += syntax_nest(syntax_kind(list.items[i].text));
    }

    syntax_free(&list);
    return depth > 0;
}
