#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

bool io_color_enabled(int fd);

// writes the whole buffer, resuming after short writes and interrupts
bool io_write_all(int fd, const void *buf, size_t len);

bool io_write_str(const char *text);
bool io_write_char(char ch);
bool io_write_repeat(char ch, size_t count);

// reads one line, dropping the terminator. returns its length, -1 at end of
// input, or -2 when the line did not fit and the remainder was discarded
ssize_t io_read_line(int fd, char *buf, size_t len);
