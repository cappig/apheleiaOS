#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

// returning true accepts the line and ends the scan
typedef bool (*textdb_line_match_fn)(const char *line, void *ctx);

const char *textdb_next_field(const char *cursor, char *out, size_t out_len);
int textdb_scan(const char *text, textdb_line_match_fn match, void *ctx);
int textdb_copy_field(char **cursor, size_t *left, char **out, const char *src);
