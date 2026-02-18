#include "psf.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <base/utf8.h>
#include <log/log.h>
#include <parse/psf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/console.h>
#include <sys/font.h>

#include "vfs.h"

static void *loaded_blob = NULL;
static size_t loaded_blob_size = 0;
static font_t loaded_font = {0};
static font_map_t *loaded_map = NULL;
static size_t loaded_map_count = 0;
static size_t loaded_map_capacity = 0;

static void _discard(void) {
    if (loaded_blob) {
        free(loaded_blob);
        loaded_blob = NULL;
    }

    if (loaded_map) {
        free(loaded_map);
        loaded_map = NULL;
    }

    loaded_blob_size = 0;
    loaded_map_count = 0;
    loaded_map_capacity = 0;

    memset(&loaded_font, 0, sizeof(loaded_font));
}

static bool _font_map_reserve(font_map_t **map, size_t *count, size_t *capacity, size_t needed) {
    if (*capacity >= needed) {
        return true;
    }

    size_t new_cap = *capacity ? *capacity * 2 : 128;

    while (new_cap < needed) {
        new_cap *= 2;
    }

    font_map_t *next = malloc(new_cap * sizeof(*next));
    if (!next) {
        return false;
    }

    if (*map) {
        memcpy(next, *map, *count * sizeof(*next));
        free(*map);
    }

    *map = next;
    *capacity = new_cap;

    return true;
}

static bool
_font_map_push(font_map_t **map, size_t *count, size_t *capacity, u32 codepoint, u32 glyph) {
    if (codepoint == 0xffff || codepoint == 0xfffe) {
        return true;
    }

    if (!_font_map_reserve(map, count, capacity, *count + 1)) {
        return false;
    }

    (*map)[(*count)++] = (font_map_t){
        .codepoint = codepoint,
        .glyph = glyph,
    };

    return true;
}

static bool _parse_psf2_unicode(
    const psf_blob_t *blob,
    font_map_t **map,
    size_t *map_count,
    size_t *map_capacity
) {
    if (!blob || !blob->unicode_table || !blob->unicode_size) {
        return true;
    }

    const u8 *table = blob->unicode_table;
    const u8 *end = table + blob->unicode_size;

    for (u32 glyph = 0; glyph < blob->glyph_count && table < end; glyph++) {
        while (table < end && *table != 0xffU) {
            if (*table == 0xfeU) {
                table++;
                continue;
            }

            u32 cp = 0;
            size_t consumed = utf8_decode(table, (size_t)(end - table), &cp);
            if (!consumed) {
                table++;
                continue;
            }

            if (!_font_map_push(map, map_count, map_capacity, cp, glyph)) {
                return false;
            }

            table += consumed;
        }

        if (table < end && *table == 0xffU) {
            table++;
        }
    }

    return true;
}

static bool _parse_psf1_unicode(
    const psf_blob_t *blob,
    font_map_t **map,
    size_t *map_count,
    size_t *map_capacity
) {
    if (!blob || !blob->unicode_table || !blob->unicode_size) {
        return true;
    }

    const u8 *table = blob->unicode_table;
    const u8 *end = table + blob->unicode_size;

    for (u32 glyph = 0; glyph < blob->glyph_count && table + 1 < end; glyph++) {
        while (table + 1 < end) {
            u16 code = (u16)(table[0] | ((u16)table[1] << 8));
            table += 2;

            if (code == 0xffffU) {
                break;
            }
            if (code == 0xfffeU) {
                continue;
            }

            if (!_font_map_push(map, map_count, map_capacity, code, glyph)) {
                return false;
            }
        }
    }

    return true;
}

bool psf_load(const char *path) {
    if (!path || !path[0]) {
        return false;
    }

    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        log_warn("font '%s' not found", path);
        return false;
    }

    if (!node->size) {
        log_warn("font '%s' is empty", path);
        return false;
    }

    void *blob = malloc((size_t)node->size);

    if (!blob) {
        return false;
    }

    ssize_t read = vfs_read(node, blob, 0, (size_t)node->size, 0);

    if (read < 0 || (size_t)read < (size_t)node->size) {
        free(blob);
        return false;
    }

    psf_blob_t blob_info = {0};
    font_map_t *map = NULL;
    size_t map_count = 0;
    size_t map_capacity = 0;

    if (!psf_parse_blob(blob, (size_t)node->size, &blob_info)) {
        free(blob);
        log_warn("failed to parse font '%s'", path);
        return false;
    }

    if (blob_info.flags & PSF_BLOB_UNICODE) {
        bool ok = true;
        if (blob_info.type == PSF_TYPE_2) {
            ok = _parse_psf2_unicode(&blob_info, &map, &map_count, &map_capacity);
        } else if (blob_info.type == PSF_TYPE_1) {
            ok = _parse_psf1_unicode(&blob_info, &map, &map_count, &map_capacity);
        }

        if (!ok) {
            free(blob);
            if (map) {
                free(map);
            }
            return false;
        }
    }

    font_t parsed = {
        .glyphs = blob_info.glyphs,
        .glyph_width = blob_info.width,
        .glyph_height = blob_info.height,
        .glyph_count = blob_info.glyph_count,
        .first_char = 0,
    };

    _discard();

    loaded_blob = blob;
    loaded_blob_size = (size_t)node->size;
    loaded_font = parsed;
    loaded_map = map;
    loaded_map_count = map_count;
    loaded_map_capacity = map_capacity;
    loaded_font.map = loaded_map;
    loaded_font.map_count = (u32)loaded_map_count;

    console_set_font(&loaded_font);

    log_debug(
        "loaded font '%s' (%ux%u, %u glyphs)",
        path,
        loaded_font.glyph_width,
        loaded_font.glyph_height,
        loaded_font.glyph_count
    );

    return true;
}
