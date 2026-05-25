#pragma once

#include <sched/scheduler.h>
#include <stdbool.h>
#include <stddef.h>

bool user_range_ok(const sched_thread_t *thread, const void *ptr, size_t len, bool write);

bool user_write_prepare(const sched_thread_t *thread, void *ptr, size_t len);

bool user_copy_from(const sched_thread_t *thread, void *dst, const void *src, size_t len);

bool user_copy_to(const sched_thread_t *thread, void *dst, const void *src, size_t len);

int user_copy_string(const sched_thread_t *thread, const char *src, char *dst, size_t dst_len);
