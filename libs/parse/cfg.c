#include "cfg.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

// this parser is not that strict. It looks for a string, then any
// number of spaces or equals signs and than another string
void parse_cfg(char *text, const cfg_entry_t *table, void *data) {
    char *tok_pos = NULL;
    char *pos = strtok_r(text, "\n", &tok_pos);

    // scan line by line
    while (pos) {
        while (isspace(*pos) && *pos) {
            pos++;
        }

        // comment or invalid line, ignore
        if (!isalnum(*pos)) {
            goto next;
        }

        // read the key
        char *line_pos = NULL;
        pos = strtok_r(pos, " =", &line_pos);
        if (!pos) {
            goto next;
        }

        char *key = pos;

        // read the value
        while (isspace(*pos) && *pos) {
            pos++;
        }

        pos = strtok_r(NULL, " =", &line_pos);
        if (!pos) {
            goto next;
        }

        char *value = pos;

        // find handler in table
        for (const cfg_entry_t *e = table; e->key; e++) {
            if (!strcasecmp(e->key, key)) {
                e->handler(value, data);
            }
        }

    next:
        pos = strtok_r(NULL, "\n", &tok_pos);
    }
}
