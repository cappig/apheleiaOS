#include "psf.h"

#include <arch/arch.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
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

typedef struct {
    font_map_t **map;
    size_t *map_count;
    size_t *map_capacity;
} font_map_builder_t;


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

static bool _font_map_reserve(
    font_map_t **map,
    size_t *count,
    size_t *capacity,
    size_t needed
) {
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

static bool _font_map_push(
    font_map_t **map,
    size_t *count,
    size_t *capacity,
    u32 codepoint,
    u32 glyph
) {
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

static bool
_collect_font_map(void *ctx, u32 codepoint, u32 glyph) {
    font_map_builder_t *builder = ctx;
    if (!builder) {
        return false;
    }

    return _font_map_push(
        builder->map,
        builder->map_count,
        builder->map_capacity,
        codepoint,
        glyph
    );
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

    font_map_builder_t builder = {
        .map = &map,
        .map_count = &map_count,
        .map_capacity = &map_capacity,
    };

    if (!psf_iter_unicode_mappings(&blob_info, _collect_font_map, &builder)) {
        free(blob);
        if (map) {
            free(map);
        }
        return false;
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
        (unsigned int)loaded_font.glyph_width,
        (unsigned int)loaded_font.glyph_height,
        (unsigned int)loaded_font.glyph_count
    );

    return true;
}

void psf_load_boot_font(const char *path) {
    size_t text_cols = 0;
    size_t text_rows = 0;
    bool had_text_grid =
        console_get_size(&text_cols, &text_rows) && text_cols && text_rows;

    if (!path || !path[0]) {
        return;
    }

    if (!psf_load(path)) {
        log_warn("failed to load console font '%s'", path);
        return;
    }

    if (!had_text_grid) {
        arch_log_replay_console();
    }
}
