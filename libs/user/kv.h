#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

ssize_t kv_read_file(const char* path, char* out, size_t out_len);
bool kv_read_string(const char* text, const char* key, char* out, size_t out_len);
bool kv_read_u64(const char* text, const char* key, unsigned long long* out);
