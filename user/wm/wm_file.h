#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

bool wm_file_read_all(const char* path, size_t max_bytes, u8** data_out, size_t* len_out);
