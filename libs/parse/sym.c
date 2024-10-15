#include "sym.h"

#include <base/types.h>
#include <stdlib.h>
#include <string.h>


// Since each line in the file maps one symbol we just have to count the lines
usize sym_count(const char* sym_file, usize sym_file_len) {
    if (!sym_file)
        return 0;

    usize lines = 0;

    for (usize i = 0; i < sym_file_len; i++)
        if (sym_file[i] == '\n')
            lines++;

    return lines;
}

// NOTE: this function modifies the file
bool sym_parse(char* sym_file, symbol_table* table) {
    if (!sym_file || !table)
        return false;

    if (!table->len || !table->map)
        return false;

    char* tok_pos = NULL;
    char* pos = strtok_r(sym_file, "\n", &tok_pos);

    for (usize i = 0; i < table->len; i++) {
        char* next_pos = strtok_r(NULL, "\n", &tok_pos);

        if (!next_pos || !pos)
            break;

        char* type;
        symbol_entry* entry = &table->map[i];

        entry->addr = strtoll(pos, &type, 16);

        entry->type = *(type + 1);

        entry->name = type + 3;

        pos = next_pos;
    }

    return true;
}
