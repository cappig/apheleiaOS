#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool changed;
    bool listed;
    bool erase_valid;
    size_t erase_start;
    size_t erase_end;
} sh_complete_result_t;

void complete_set_path(const char* path);

void complete_line(
    char* buf,
    size_t cap,
    size_t* len,
    size_t* cursor,
    sh_complete_result_t* result
);
