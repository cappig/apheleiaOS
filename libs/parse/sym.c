#include "sym.h"

#include <base/types.h>
#include <stdlib.h>
#include <string.h>


// Since each line in the file maps one symbol we just have to count the lines
// TODO: this should be a more generic function in a lib
size_t sym_count(const char *sym_file, size_t sym_file_len) {
    if (!sym_file) {
        return 0;
    }

    size_t lines = 0;

    for (size_t i = 0; i < sym_file_len; i++) {
        if (sym_file[i] == '\n') {
            lines++;
        }
    }

    return lines;
}

// NOTE: this function modifies the file
bool sym_parse(char *sym_file, symbol_table_t *table) {
    if (!sym_file || !table) {
        return false;
    }

    if (!table->len || !table->map) {
        return false;
    }

    char *tok_pos = NULL;
    char *pos = strtok_r(sym_file, "\n", &tok_pos);

    for (size_t i = 0; i < table->len; i++) {
        char *next_pos = strtok_r(NULL, "\n", &tok_pos);

        if (!next_pos || !pos) {
            break;
        }

        char *type;
        symbol_entry_t *entry = &table->map[i];

        entry->addr = strtoll(pos, &type, 16);

        entry->type = *(type + 1);

        entry->name = type + 3;

        pos = next_pos;
    }

    return true;
}
