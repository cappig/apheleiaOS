#pragma once

#include <base/types.h>

typedef struct {
    u64 addr;
    char *name;
} symbol_entry_t;

typedef struct {
    size_t len;
    symbol_entry_t *map;
} symbol_table_t;

void load_symbols(void);

symbol_entry_t *resolve_symbol(u64 addr);
const char *resolve_symbol_name(u64 addr);
