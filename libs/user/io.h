#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

bool io_color_enabled(int fd);
ssize_t io_write_str(const char *text);
ssize_t io_write_char(char ch);
ssize_t io_write_repeat(char ch, size_t count);
