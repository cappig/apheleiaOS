#pragma once

#include <base/types.h>
#include <stddef.h>

#define SYSCALL_STAT_MAX 64

typedef struct {
    u64 number;
    const char *name;
    u64 calls;
    u64 errors;
} syscall_stat_t;

size_t syscall_stats_snapshot(syscall_stat_t *stats, size_t max_stats, u64 *unknown_out);
void syscall_init(void);
