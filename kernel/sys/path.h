#pragma once

#include <base/types.h>
#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

bool path_resolve(const char* cwd, const char* path, char* out, size_t out_len);
