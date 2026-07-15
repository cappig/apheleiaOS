#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SCRIPT_MAX_STMTS 256
#define SCRIPT_MAX_WORDS 32

typedef struct {
    char *text;
} script_stmt_t;

typedef struct {
    script_stmt_t items[SCRIPT_MAX_STMTS];
    int count;
    bool incomplete;
} script_list_t;

typedef enum {
    STMT_SIMPLE,
    STMT_IF,
    STMT_THEN,
    STMT_ELIF,
    STMT_ELSE,
    STMT_FI,
    STMT_WHILE,
    STMT_FOR,
    STMT_DO,
    STMT_DONE,
    STMT_CASE,
    STMT_ESAC,
    STMT_OPEN_BRACE,
    STMT_CLOSE_BRACE,
    STMT_CASE_END,
} stmt_kind_t;

typedef void (*syntax_error_fn)(const char *message);

char *syntax_trim(char *text);
bool syntax_starts(const char *text, const char *word);
bool syntax_name(const char *name, size_t len);
size_t syntax_sub_len(const char *text);

bool syntax_split(const char *source, script_list_t *list, syntax_error_fn error);
void syntax_free(script_list_t *list);

stmt_kind_t syntax_kind(const char *text);
int syntax_nest(stmt_kind_t kind);
int syntax_find(script_list_t *list, int start, int end, unsigned int wanted);

int syntax_words(char *text, char **words, int max_words);
char *syntax_case_end(char *text);
