#include "psf.h"

#include <base/attributes.h>
#include <base/utf8.h>
#include <string.h>

#define PSF1_MAGIC 0x0436U
#define PSF2_MAGIC 0x864ab572U

#define PSF1_MODE_512     0x01U
#define PSF1_MODE_UNICODE 0x02U

#define PSF2_MODE_UNICODE 0x01U

typedef struct PACKED {
    u16 magic;
    u8 mode;
    u8 char_size;
} psf1_header_t;

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

static u32 read_u32_le(const u8 *ptr) {
    return (u32)ptr[0] | ((u32)ptr[1] << 8) | ((u32)ptr[2] << 16) | ((u32)ptr[3] << 24);
}

bool psf_parse_blob(const void *data, size_t size, psf_blob_t *out) {
    if (!data || !out || size < sizeof(psf1_header_t)) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    const u8 *bytes = data;

    if (size >= sizeof(psf2_header_t) && read_u32_le(bytes) == PSF2_MAGIC) {
        const psf2_header_t *psf2 = data;

        if (psf2->header_size < sizeof(psf2_header_t)) {
            return false;
        }

        if (!psf2->glyph_count || !psf2->glyph_bytes || !psf2->width || !psf2->height) {
            return false;
        }

        size_t glyphs_size =
            (size_t)psf2->glyph_count * (size_t)psf2->glyph_bytes;

        size_t need = (size_t)psf2->header_size + glyphs_size;

        if (need > size) {
            return false;
        }

        out->type = PSF_TYPE_2;
        out->width = psf2->width;
        out->height = psf2->height;
        out->glyph_count = psf2->glyph_count;
        out->glyph_size = psf2->glyph_bytes;
        out->row_bytes = (psf2->width + 7U) / 8U;
        out->glyphs = bytes + psf2->header_size;

        if (psf2->flags & PSF2_MODE_UNICODE) {
            out->flags |= PSF_BLOB_UNICODE;
            out->unicode_table = out->glyphs + glyphs_size;
            out->unicode_size = size - need;
        }

        return true;
    }

    const psf1_header_t *psf1 = data;
    if (psf1->magic != PSF1_MAGIC || !psf1->char_size) {
        return false;
    }

    u32 glyph_count = (psf1->mode & PSF1_MODE_512) ? 512U : 256U;
    size_t glyphs_size = (size_t)glyph_count * psf1->char_size;
    size_t header_size = sizeof(psf1_header_t);
    size_t need = header_size + glyphs_size;

    if (need > size) {
        return false;
    }

    out->type = PSF_TYPE_1;
    out->width = 8;
    out->height = psf1->char_size;
    out->glyph_count = glyph_count;
    out->glyph_size = psf1->char_size;
    out->row_bytes = 1;
    out->glyphs = bytes + header_size;

    if (psf1->mode & PSF1_MODE_UNICODE) {
        out->flags |= PSF_BLOB_UNICODE;
        out->unicode_table = out->glyphs + glyphs_size;
        out->unicode_size = size - need;
    }

    return true;
}

bool psf_iter_unicode_mappings(
    const psf_blob_t *blob,
    psf_unicode_map_iter_t iter,
    void *ctx
) {
    if (!blob || !iter) {
        return false;
    }

    if (
        !(blob->flags & PSF_BLOB_UNICODE) ||
        !blob->unicode_table ||
        !blob->unicode_size
    ) {
        return true;
    }

    const u8 *table = blob->unicode_table;
    const u8 *end = table + blob->unicode_size;

    if (blob->type == PSF_TYPE_2) {
        for (u32 glyph = 0; glyph < blob->glyph_count && table < end; glyph++) {
            bool sequence = false;

            while (table < end && *table != 0xffU) {
                if (*table == 0xfeU) {
                    sequence = true;
                    table++;
                    continue;
                }

                u32 codepoint = 0;
                size_t consumed =
                    utf8_decode(table, (size_t)(end - table), &codepoint);

                if (!consumed) {
                    table++;
                    continue;
                }

                if (
                    !sequence &&
                    codepoint != 0xfffeU &&
                    codepoint != 0xffffU &&
                    !iter(ctx, codepoint, glyph)
                ) {
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

    if (blob->type == PSF_TYPE_1) {
        for (u32 glyph = 0; glyph < blob->glyph_count && table + 1 < end; glyph++) {
            bool sequence = false;

            while (table + 1 < end) {
                u16 code = (u16)(table[0] | ((u16)table[1] << 8));
                table += 2;

                if (code == 0xffffU) {
                    break;
                }

                if (code == 0xfffeU) {
                    sequence = true;
                    continue;
                }

                if (!sequence && !iter(ctx, code, glyph)) {
                    return false;
                }
            }
        }

        return true;
    }

    return false;
}
