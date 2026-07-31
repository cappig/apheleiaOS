#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

bool io_color_enabled(int fd);
bool io_write_all(int fd, const void *buf, size_t len);
bool io_write_str(const char *text);
bool io_write_char(char ch);
bool io_write_repeat(char ch, size_t count);

// drops the terminator and returns the length, -1 at end of input, -2 if too long
ssize_t io_read_line(int fd, char *buf, size_t len);
