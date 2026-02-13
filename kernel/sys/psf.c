#include "psf.h"

#include <arch/arch.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <base/utf8.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/font.h>

#include "vfs.h"

#define PSF1_MAGIC 0x0436
#define PSF2_MAGIC 0x864ab572
#define PSF1_WIDTH 8

typedef struct PACKED {
    u16 magic;
    u8 mode;
    u8 char_size;
} psf1_header_t;

enum psf1_modes {
    PSF1_MODE_512 = 0x01,
    PSF1_MODE_UNICODE = 0x02,
};

typedef struct PACKED {
    u32 magic;
    u32 version;
    u32 header_size;
    u32 flags;
    u32 glyph_count;
    u32 glyph_bytes;
    u32 height;
    u32 width;
} psf2_header_t;

enum psf2_modes {
    PSF2_MODE_UNICODE = 0x01,
};

static void* loaded_blob = NULL;
static size_t loaded_blob_size = 0;
static font_t loaded_font = {0};
static font_map_t* loaded_map = NULL;
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

static bool _font_map_reserve(font_map_t** map, size_t* count, size_t* capacity, size_t needed) {
    if (*capacity >= needed)
        return true;

    size_t new_cap = *capacity ? *capacity * 2 : 128;

    while (new_cap < needed)
        new_cap *= 2;

    font_map_t* next = malloc(new_cap * sizeof(*next));
    if (!next)
        return false;

    if (*map) {
        memcpy(next, *map, *count * sizeof(*next));
        free(*map);
    }

    *map = next;
    *capacity = new_cap;

    return true;
}

static bool
_font_map_push(font_map_t** map, size_t* count, size_t* capacity, u32 codepoint, u32 glyph) {
    if (codepoint == 0xffff || codepoint == 0xfffe)
        return true;

    if (!_font_map_reserve(map, count, capacity, *count + 1))
        return false;

    (*map)[(*count)++] = (font_map_t){
        .codepoint = codepoint,
        .glyph = glyph,
    };

    return true;
}

static bool _parse(
    const void* data,
    size_t size,
    font_t* out,
    font_map_t** map,
    size_t* map_count,
    size_t* map_capacity
) {
    if (!data || !out || size < sizeof(psf1_header_t))
        return false;

    const psf2_header_t* psf2 = data;

    if (psf2->magic == PSF2_MAGIC) {
        if (psf2->header_size < sizeof(psf2_header_t))
            return false;

        if (!psf2->glyph_count || !psf2->glyph_bytes)
            return false;

        size_t glyphs_size = (size_t)psf2->glyph_count * psf2->glyph_bytes;

        if (psf2->header_size + glyphs_size > size)
            return false;

        const u8* glyphs = (const u8*)data + psf2->header_size;

        out->glyphs = glyphs;
        out->glyph_width = psf2->width;
        out->glyph_height = psf2->height;
        out->glyph_count = psf2->glyph_count;
        out->first_char = 0;

        if (psf2->flags & PSF2_MODE_UNICODE) {
            const u8* table = glyphs + glyphs_size;
            const u8* end = (const u8*)data + size;

            for (u32 glyph = 0; glyph < psf2->glyph_count && table < end; glyph++) {
                while (table < end && *table != 0xff) {
                    if (*table == 0xfe) {
                        table++;
                        continue;
                    }

                    u32 cp = 0;
                    size_t consumed = utf8_decode(table, (size_t)(end - table), &cp);

                    if (!consumed) {
                        table++;
                        continue;
                    }

                    if (!_font_map_push(map, map_count, map_capacity, cp, glyph))
                        return false;

                    table += consumed;
                }

                if (table < end && *table == 0xff)
                    table++;
            }
        }

        return true;
    }

    const psf1_header_t* psf1 = data;
    if (psf1->magic != PSF1_MAGIC)
        return false;

    u32 glyph_count = (psf1->mode & PSF1_MODE_512) ? 512 : 256;
    size_t glyph_size = psf1->char_size;
    size_t glyphs_size = glyph_size * glyph_count;
    size_t header_size = sizeof(psf1_header_t);

    if (header_size + glyphs_size > size)
        return false;

    out->glyphs = (const u8*)data + header_size;
    out->glyph_width = PSF1_WIDTH;
    out->glyph_height = psf1->char_size;
    out->glyph_count = glyph_count;
    out->first_char = 0;

    if (psf1->mode & PSF1_MODE_UNICODE) {
        const u8* table = (const u8*)data + header_size + glyphs_size;
        const u8* end = (const u8*)data + size;

        for (u32 glyph = 0; glyph < glyph_count && table + 1 < end; glyph++) {
            while (table + 1 < end) {
                u16 code = (u16)(table[0] | (table[1] << 8));

                table += 2;

                if (code == 0xffff)
                    break;

                if (code == 0xfffe)
                    continue;

                if (!_font_map_push(map, map_count, map_capacity, code, glyph))
                    return false;
            }
        }
    }

    return true;
}

bool psf_load(const char* path) {
    if (!path || !path[0])
        return false;

    vfs_node_t* node = vfs_lookup(path);
    if (!node) {
        log_warn("console: font '%s' not found", path);
        return false;
    }

    if (!node->size) {
        log_warn("console: font '%s' is empty", path);
        return false;
    }

    void* blob = malloc((size_t)node->size);

    if (!blob)
        return false;

    ssize_t read = vfs_read(node, blob, 0, (size_t)node->size, 0);

    if (read < 0 || (size_t)read < (size_t)node->size) {
        free(blob);
        return false;
    }

    font_t parsed = {0};
    font_map_t* map = NULL;
    size_t map_count = 0;
    size_t map_capacity = 0;

    if (!psf_parse(blob, (size_t)node->size, &parsed, &map, &map_count, &map_capacity)) {
        free(blob);

        if (map)
            free(map);

        log_warn("console: failed to parse font '%s'", path);

        return false;
    }

    psf_discard();

    loaded_blob = blob;
    loaded_blob_size = (size_t)node->size;
    loaded_font = parsed;
    loaded_map = map;
    loaded_map_count = map_count;
    loaded_map_capacity = map_capacity;
    loaded_font.map = loaded_map;
    loaded_font.map_count = (u32)loaded_map_count;

    arch_set_font(&loaded_font);

    log_info(
        "console: loaded font '%s' (%ux%u, %u glyphs)",
        path,
        loaded_font.glyph_width,
        loaded_font.glyph_height,
        loaded_font.glyph_count
    );

    return true;
}
