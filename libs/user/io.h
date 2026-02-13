#pragma once

#include <stddef.h>
#include <sys/types.h>

ssize_t io_write_str(const char* text);
ssize_t io_write_char(char ch);
