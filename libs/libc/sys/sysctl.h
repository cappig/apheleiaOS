#pragma once

#include <stddef.h>
#include <sys/types.h>

ssize_t sysctl(const char* name, void* out, size_t len);
