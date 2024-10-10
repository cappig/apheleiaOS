#pragma once

#include <base/attributes.h>
#include <base/types.h>

// The PC screen font
// https://en.wikipedia.org/wiki/PC_Screen_Font
// https://wiki.osdev.org/PC_Screen_Font

#define PSF1_MAGIC 0x0436
#define PSF1_WIDTH 8

typedef struct PACKED {
    u16 magic;
    u8 font_mode;
    u8 char_size;
} psf1_header;

enum psf1_modes {
    PSF1_MODE_512 = 0x01, // if set we have 512 glyphs, if not 256
    PSF1_MODE_UNICODE = 0x02, // do we have a unicode table
    PSF1_MODE_SEQ = 0x04, // equivalent to PSF1_MODE_UNICODE, dumb
};

#define PSF2_MAGIC 0x864ab572

typedef struct PACKED {
    u32 magic;
    u32 version;
    u32 header_size;
    u32 flags;
    u32 glyph_count;
    u32 glyph_bytes;
    u32 height;
    u32 width;
} psf2_header;

enum psf2_modes {
    PSF2_MODE_UNICODE = 0x01, // do we have a unicode table
};


typedef enum {
    PSF_FONT_NONE,
    PSF_FONT_PSF1,
    PSF_FONT_PSF2,
} psf_font_type;

typedef struct {
    psf_font_type type;
    psf2_header* header;

    u8* data;

    usize glyph_width;
    usize glyph_height;

    usize glyph_size;
} psf_font;
