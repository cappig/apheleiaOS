#pragma once

#include <base/types.h>
#include <parse/sym.h>


void load_symbols(void);

isize resolve_symbol(u64 addr);
char* resolve_symbol_name(u64 addr);

symbol_entry* get_symbol(usize index);
