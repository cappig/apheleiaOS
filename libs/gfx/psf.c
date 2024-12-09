#include "psf.h"


bool psf_parse(void* bin, psf_font* font) {
    if (!bin)
        return false;

    psf2_header* psf2_head = bin;

    if (psf2_head->magic == PSF2_MAGIC) {
        font->type = PSF_FONT_PSF2;
        font->data = bin + sizeof(psf2_header);

        font->glyph_height = psf2_head->height;
        font->glyph_width = psf2_head->width;
        font->glyph_size = psf2_head->glyph_bytes;

        font->header = bin;

        return true;
    }

    psf1_header* psf1_head = bin;

    if (psf1_head->magic == PSF1_MAGIC) {
        font->type = PSF_FONT_PSF1;
        font->data = bin + sizeof(psf1_header);

        font->glyph_height = psf1_head->char_size;
        font->glyph_width = PSF1_WIDTH;
        font->glyph_size = psf1_head->char_size;

        font->header = bin;

        return true;
    }

    return false;
}
