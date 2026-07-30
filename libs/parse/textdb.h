#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

// returns true once a line has been accepted, which ends the scan
typedef bool (*textdb_line_match_fn)(const char *line, void *ctx);

const char *textdb_next_field(const char *cursor, char *out, size_t out_len);

// offers every usable line of a colon separated database to match. blank,
// commented and over-long lines are skipped. returns 0 once a line is
// accepted, or -1 if nothing matched
int textdb_scan(const char *text, textdb_line_match_fn match, void *ctx);

// appends src to a caller supplied scratch buffer and points *out at the copy,
// advancing the cursor. returns ERANGE when the buffer is exhausted
int textdb_copy_field(char **cursor, size_t *left, char **out, const char *src);
