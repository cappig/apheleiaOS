#include "cfg.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>


// This parser is not that strict. It looks for a string, then any
// number of spaces or equals signs and than another string
void parse_cfg(char* text, cfg_comp_fn comp, void* data) {
    char* tok_pos = NULL;
    char* pos = strtok_r(text, "\n", &tok_pos);

    // Scan line by line
    while (pos) {
        while (isspace(*pos) && *pos)
            pos++;

        // Comment or invalid line, ignore
        if (!isalnum(*pos))
            goto next;

        // Read the key
        char* line_pos = NULL;
        pos = strtok_r(pos, " =", &line_pos);
        if (!pos)
            goto next;

        char* key = pos;

        // Read the value
        while (isspace(*pos) && *pos)
            pos++;

        pos = strtok_r(NULL, " =", &line_pos);
        if (!pos)
            goto next;

        char* value = pos;

        comp(key, value, data);

    next:
        pos = strtok_r(NULL, "\n", &tok_pos);
    }
}
