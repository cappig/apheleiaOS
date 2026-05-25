#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

bool fs_is_dir_mode(mode_t mode);
const char *fs_path_basename(const char *path);
void fs_join_path(char *out, size_t out_len, const char *left, const char *right);

void fs_format_mode(mode_t mode, char out[11]);
void fs_format_time_short(time_t t, char *out, size_t out_len);
