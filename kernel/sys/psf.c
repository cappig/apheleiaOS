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

typedef struct {
    void *blob;
    font_t font;
    font_map_t *map;
} psf_loaded_font_t;

typedef struct {
    font_map_t *items;
    size_t count;
    size_t cap;
} font_map_builder_t;

static psf_loaded_font_t current_font = { 0 };

static void _discard_loaded_font(void) {
    if (current_font.blob) {
        free(current_font.blob);
    }

    if (current_font.map) {
        free(current_font.map);
    }

    memset(&current_font, 0, sizeof(current_font));
}

static bool _font_map_reserve(font_map_builder_t *builder, size_t needed) {
    if (builder->cap >= needed) {
        return true;
    }

    size_t new_cap = builder->cap ? builder->cap * 2 : 128;

    while (new_cap < needed) {
        new_cap *= 2;
    }

    font_map_t *next = malloc(new_cap * sizeof(*next));
    if (!next) {
        return false;
    }

    if (builder->items) {
        memcpy(next, builder->items, builder->count * sizeof(*next));
        free(builder->items);
    }

    builder->items = next;
    builder->cap = new_cap;

    return true;
}

static bool _font_map_push(font_map_builder_t *builder, u32 codepoint, u32 glyph) {
    if (codepoint == 0xffff || codepoint == 0xfffe) {
        return true;
    }

    if (!_font_map_reserve(builder, builder->count + 1)) {
        return false;
    }

    builder->items[builder->count++] = (font_map_t){
        .codepoint = codepoint,
        .glyph = glyph,
    };

    return true;
}

static bool _collect_font_map(void *ctx, u32 codepoint, u32 glyph) {
    font_map_builder_t *builder = ctx;
    if (!builder) {
        return false;
    }

    return _font_map_push(builder, codepoint, glyph);
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

    psf_blob_t blob_info = { 0 };
    font_map_builder_t map = { 0 };

    if (!psf_parse_blob(blob, (size_t)node->size, &blob_info)) {
        free(blob);
        log_warn("failed to parse font '%s'", path);
        return false;
    }

    if (!psf_iter_unicode_mappings(&blob_info, _collect_font_map, &map)) {
        free(blob);
        if (map.items) {
            free(map.items);
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

    _discard_loaded_font();

    current_font.blob = blob;
    current_font.font = parsed;
    current_font.map = map.items;
    current_font.font.map = current_font.map;
    current_font.font.map_count = (u32)map.count;

    console_set_font(&current_font.font);

    log_debug(
        "loaded font '%s' (%ux%u, %u glyphs)",
        path,
        (unsigned int)current_font.font.glyph_width,
        (unsigned int)current_font.font.glyph_height,
        (unsigned int)current_font.font.glyph_count
    );

    return true;
}

void psf_load_boot_font(const char *path) {
    size_t text_cols = 0;
    size_t text_rows = 0;
    bool had_text_grid = console_get_size(&text_cols, &text_rows) && text_cols && text_rows;

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
