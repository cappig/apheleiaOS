#pragma once

#include <base/types.h>
#include <parse/sym.h>

void load_symbols(void);

symbol_entry_t* resolve_symbol(u64 addr);
const char* resolve_symbol_name(u64 addr);
