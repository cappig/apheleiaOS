#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int (*run_command)(const char *command);
    bool (*expand_word)(const char *word, char *out, size_t out_len);
    bool (*set_var)(const char *name, const char *value);
    int (*arg_count)(void);
    const char *(*arg_at)(int index);
    void (*report_error)(const char *message);
} sh_script_ops_t;

void script_init(const sh_script_ops_t *ops);

int script_run(const char *source);
int script_call(const char *name);

bool script_has_function(const char *name);
bool script_needs_more(const char *source);
