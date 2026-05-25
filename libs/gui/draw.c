#include "draw.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <psf.h>
#include <stdlib.h>
#include <string.h>
#include <term/glyph.h>
#include <unistd.h>
#include <user/kv.h>

#define DRAW_FONT_BUF_SIZE      (256 * 1024)
#define DRAW_FONT_PATH_DEFAULT  "/etc/font.psf"
#define DRAW_WM_CONFIG_PATH     "/etc/wm.conf"
#define DRAW_WM_CONFIG_BUF_SIZE 2048
#if defined(PATH_MAX)
#define DRAW_FONT_PATH_MAX PATH_MAX
#else
#define DRAW_FONT_PATH_MAX 4096
#endif

typedef struct {
    u32 codepoint;
    u32 glyph;
} draw_font_map_entry_t;

typedef struct {
    u8 buf[DRAW_FONT_BUF_SIZE];
    psf_font_t psf;

    draw_font_map_entry_t *map;
    size_t map_count;
    size_t map_cap;

    u32 cell_width;
    u32 cell_height;
    u32 cell_src_x;

    bool load_attempted;
    bool loaded;
    bool path_ready;
    char path[DRAW_FONT_PATH_MAX];
} draw_font_cache_t;

static draw_font_cache_t font_cache = {
    .cell_width = 8,
    .cell_height = 16,
    .path = DRAW_FONT_PATH_DEFAULT,
};

static i32 min3(i32 a, i32 b, i32 c) {
    i32 min = a;
    if (b < min) {
        min = b;
    }
    if (c < min) {
        min = c;
    }
    return min;
}

static i32 max3(i32 a, i32 b, i32 c) {
    i32 max = a;
    if (b > max) {
        max = b;
    }
    if (c > max) {
        max = c;
    }
    return max;
}

static i64 edge(draw_point_t a, draw_point_t b, i32 px, i32 py) {
    return (i64)(px - a.x) * (i64)(b.y - a.y) - (i64)(py - a.y) * (i64)(b.x - a.x);
}

static size_t _stride_pixels(const framebuffer_t *fb) {
    if (!fb || !fb->width) {
        return 0;
    }

    if (!fb->stride) {
        return fb->width;
    }

    size_t stride = (size_t)fb->stride / sizeof(pixel_t);
    if (!stride || stride < fb->width) {
        return fb->width;
    }

    return stride;
}

static bool _glyph_pixel_on(const u8 *glyph, u32 row_bytes, u32 x, u32 y) {
    const u8 *row_ptr = glyph + (size_t)y * row_bytes;
    u8 bits = row_ptr[x / 8];
    u8 mask = (u8)(0x80 >> (x & 7));
    return (bits & mask) != 0;
}

static void _clear_font_map(void) {
    if (!font_cache.map) {
        return;
    }

    free(font_cache.map);
    font_cache.map = NULL;
    font_cache.map_count = 0;
    font_cache.map_cap = 0;
}

static void _reset_draw_font_state(void) {
    memset(&font_cache.psf, 0, sizeof(font_cache.psf));
    _clear_font_map();
    font_cache.cell_src_x = 0;
    font_cache.cell_width = 8U;
    font_cache.cell_height = 16U;
    font_cache.load_attempted = false;
    font_cache.loaded = false;
}

static bool _reserve_font_map(size_t needed) {
    if (font_cache.map_cap >= needed) {
        return true;
    }

    size_t new_cap = font_cache.map_cap ? font_cache.map_cap * 2 : 128;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    draw_font_map_entry_t *next = malloc(new_cap * sizeof(*next));
    if (!next) {
        return false;
    }

    if (font_cache.map && font_cache.map_count) {
        memcpy(next, font_cache.map, font_cache.map_count * sizeof(*next));
    }

    free(font_cache.map);
    font_cache.map = next;
    font_cache.map_cap = new_cap;
    return true;
}

static bool _push_font_map(u32 codepoint, u32 glyph) {
    if (!_reserve_font_map(font_cache.map_count + 1)) {
        return false;
    }

    font_cache.map[font_cache.map_count++] = (draw_font_map_entry_t){
        .codepoint = codepoint,
        .glyph = glyph,
    };
    return true;
}

static bool _push_font_map_iter(void *ctx, u32 codepoint, u32 glyph) {
    (void)ctx;
    return _push_font_map(codepoint, glyph);
}

static void _sort_font_map(void) {
    for (size_t i = 1; i < font_cache.map_count; i++) {
        draw_font_map_entry_t current = font_cache.map[i];
        size_t j = i;

        while (j > 0 && font_cache.map[j - 1].codepoint > current.codepoint) {
            font_cache.map[j] = font_cache.map[j - 1];
            j--;
        }

        font_cache.map[j] = current;
    }
}

static bool _find_mapped_glyph(u32 codepoint, u32 *glyph_out) {
    if (!glyph_out || !font_cache.map || !font_cache.map_count) {
        return false;
    }

    size_t lo = 0;
    size_t hi = font_cache.map_count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const draw_font_map_entry_t *entry = &font_cache.map[mid];

        if (entry->codepoint == codepoint) {
            *glyph_out = entry->glyph;
            return true;
        }

        if (entry->codepoint < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return false;
}

static u32 _glyph_index_for(u32 codepoint) {
    u32 glyph = 0;

    if (_find_mapped_glyph(codepoint, &glyph)) {
        if (glyph < font_cache.psf.glyph_count) {
            return glyph;
        }
    }

    if (!font_cache.map_count && codepoint < font_cache.psf.glyph_count) {
        return codepoint;
    }

    if (_find_mapped_glyph((u32)'?', &glyph)) {
        if (glyph < font_cache.psf.glyph_count) {
            return glyph;
        }
    }

    if (!font_cache.map_count && (u32)'?' < font_cache.psf.glyph_count) {
        return (u32)'?';
    }

    return 0;
}

static bool _glyph_bounds_for_index(u32 glyph_idx, u32 *left_out, u32 *right_out) {
    if (glyph_idx >= font_cache.psf.glyph_count || !left_out || !right_out) {
        return false;
    }

    const u8 *glyph = font_cache.psf.glyphs + (size_t)glyph_idx * font_cache.psf.glyph_size;
    u32 left = font_cache.psf.width;
    u32 right = 0;
    bool any = false;

    for (u32 gy = 0; gy < font_cache.psf.height; gy++) {
        for (u32 gx = 0; gx < font_cache.psf.width; gx++) {
            if (!_glyph_pixel_on(glyph, font_cache.psf.row_bytes, gx, gy)) {
                continue;
            }

            if (gx < left) {
                left = gx;
            }

            if (!any || gx > right) {
                right = gx;
            }

            any = true;
        }
    }

    if (!any) {
        return false;
    }

    *left_out = left;
    *right_out = right;

    return true;
}

static void _derive_font_metrics(void) {
    font_cache.cell_src_x = 0;
    font_cache.cell_width = font_cache.psf.width ? font_cache.psf.width : 8U;
    font_cache.cell_height = font_cache.psf.height ? font_cache.psf.height : 16U;

    if (!font_cache.psf.glyphs || !font_cache.psf.width || !font_cache.psf.height || !font_cache.psf.row_bytes) {
        return;
    }

    u32 min_left = font_cache.psf.width;
    u32 max_right = 0;
    bool have_bounds = false;

    // Base spacing on printable ASCII to keep text layout stable.
    for (u32 cp = 32; cp < 127; cp++) {
        u32 glyph = 0;
        if (font_cache.map_count) {
            if (!_find_mapped_glyph(cp, &glyph)) {
                continue;
            }
        } else {
            glyph = _glyph_index_for(cp);
        }

        u32 left = 0;
        u32 right = 0;
        if (!_glyph_bounds_for_index(glyph, &left, &right)) {
            continue;
        }

        if (left < min_left) {
            min_left = left;
        }

        if (!have_bounds || right > max_right) {
            max_right = right;
        }

        have_bounds = true;
    }

    if (!have_bounds || max_right < min_left) {
        return;
    }

    u32 advance = max_right - min_left + 1;
    u32 max_advance = font_cache.psf.width - min_left;

    if (!advance || !max_advance) {
        return;
    }

    if (advance < max_advance) {
        u32 padded = advance + TERM_GLYPH_CELL_GAP_PX;
        if (padded < advance || padded > max_advance) {
            padded = max_advance;
        }
        advance = padded;
    }

    font_cache.cell_src_x = min_left;
    font_cache.cell_width = advance;
}

static void _resolve_draw_font_path(void) {
    if (font_cache.path_ready) {
        return;
    }

    font_cache.path_ready = true;

    char cfg_text[DRAW_WM_CONFIG_BUF_SIZE];
    int cfg_fd = open(DRAW_WM_CONFIG_PATH, O_RDONLY, 0);
    if (cfg_fd < 0) {
        return;
    }

    ssize_t cfg_len = kv_read_fd(cfg_fd, cfg_text, sizeof(cfg_text));
    close(cfg_fd);
    if (cfg_len <= 0) {
        return;
    }

    char configured[DRAW_FONT_PATH_MAX] = { 0 };
    if (!kv_read_string(cfg_text, "font", configured, sizeof(configured))) {
        return;
    }

    char *start = configured;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    if (!start[0]) {
        return;
    }

    size_t len = strlen(start);
    if (len >= sizeof(font_cache.path)) {
        return;
    }

    memcpy(font_cache.path, start, len + 1);
}

static bool _load_draw_font(void) {
    if (font_cache.load_attempted) {
        return font_cache.loaded;
    }

    _resolve_draw_font_path();
    font_cache.load_attempted = true;
    _clear_font_map();

    if (psf_load_file(font_cache.path, font_cache.buf, sizeof(font_cache.buf), &font_cache.psf)) {
        if (!psf_iter_unicode_mappings(&font_cache.psf, _push_font_map_iter, NULL)) {
            memset(&font_cache.psf, 0, sizeof(font_cache.psf));
            _clear_font_map();
            font_cache.loaded = false;
            return false;
        }

        _sort_font_map();
        _derive_font_metrics();
        font_cache.loaded = true;
        return true;
    }

    memset(&font_cache.psf, 0, sizeof(font_cache.psf));
    _clear_font_map();
    font_cache.cell_src_x = 0;
    font_cache.cell_width = 8U;
    font_cache.cell_height = 16U;
    font_cache.loaded = false;
    return false;
}

bool draw_set_font_path(const char *path) {
    if (!path || !path[0]) {
        return false;
    }

    size_t len = strlen(path);
    if (len >= sizeof(font_cache.path)) {
        return false;
    }

    if (!strcmp(font_cache.path, path)) {
        font_cache.path_ready = true;
        return true;
    }

    memcpy(font_cache.path, path, len + 1);
    font_cache.path_ready = true;
    _reset_draw_font_state();
    return true;
}

const char *draw_get_font_path(void) {
    _resolve_draw_font_path();
    if (!font_cache.path[0]) {
        return DRAW_FONT_PATH_DEFAULT;
    }
    return font_cache.path;
}

u32 draw_font_width(void) {
    if (_load_draw_font() && font_cache.cell_width) {
        return font_cache.cell_width;
    }

    return 8U;
}

u32 draw_font_height(void) {
    if (_load_draw_font() && font_cache.cell_height) {
        return font_cache.cell_height;
    }

    return 16U;
}

static inline void _put_pixel(framebuffer_t *fb, size_t stride_pixels, i32 x, i32 y, pixel_t color) {
    if (!fb || !fb->pixels || x < 0 || y < 0) {
        return;
    }

    if (x >= (i32)fb->width || y >= (i32)fb->height) {
        return;
    }

    fb->pixels[(size_t)y * stride_pixels + (size_t)x] = color;
}

static inline void _draw_hspan(framebuffer_t *fb, size_t stride_pixels, i32 x0, i32 x1, i32 y, pixel_t color) {
    if (!fb || !fb->pixels || y < 0 || y >= (i32)fb->height) {
        return;
    }

    if (x0 > x1) {
        i32 tmp = x0;
        x0 = x1;
        x1 = tmp;
    }

    if (x1 < 0 || x0 >= (i32)fb->width) {
        return;
    }

    if (x0 < 0) {
        x0 = 0;
    }

    if (x1 >= (i32)fb->width) {
        x1 = (i32)fb->width - 1;
    }

    size_t start = (size_t)y * stride_pixels + (size_t)x0;
    size_t count = (size_t)(x1 - x0 + 1);
    pixel_t *row = fb->pixels + start;

    for (size_t i = 0; i < count; i++) {
        row[i] = color;
    }
}

void draw_rect(framebuffer_t *fb, i32 x, i32 y, u32 width, u32 height, pixel_t color) {
    if (!fb || !fb->pixels || !fb->width || !fb->height || !width || !height) {
        return;
    }

    size_t stride_pixels = _stride_pixels(fb);
    if (!stride_pixels) {
        return;
    }

    i32 x0 = x;
    i32 y0 = y;
    i32 x1 = x + (i32)width;
    i32 y1 = y + (i32)height;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (i32)fb->width) {
        x1 = (i32)fb->width;
    }
    if (y1 > (i32)fb->height) {
        y1 = (i32)fb->height;
    }

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    size_t span = (size_t)(x1 - x0);
    size_t span_bytes = span * sizeof(pixel_t);

    // Check if all 4 bytes are the same — memset is fastest for this case
    u8 b0 = (u8)color;
    bool byte_fill = (b0 == (u8)(color >> 8)) && (b0 == (u8)(color >> 16)) && (b0 == (u8)(color >> 24));

    if (byte_fill) {
        for (i32 row = y0; row < y1; row++) {
            memset(fb->pixels + (size_t)row * stride_pixels + (size_t)x0, (int)b0, span_bytes);
        }
        return;
    }

    // Fill the first row manually, then memcpy to subsequent rows
    pixel_t *first_row = fb->pixels + (size_t)y0 * stride_pixels + (size_t)x0;

    for (size_t i = 0; i < span; i++) {
        first_row[i] = color;
    }

    for (i32 row = y0 + 1; row < y1; row++) {
        memcpy(fb->pixels + (size_t)row * stride_pixels + (size_t)x0, first_row, span_bytes);
    }
}

void draw_line(framebuffer_t *fb, i32 x0, i32 y0, i32 x1, i32 y1, pixel_t color) {
    if (!fb || !fb->pixels || !fb->width || !fb->height) {
        return;
    }

    size_t stride_pixels = _stride_pixels(fb);
    if (!stride_pixels) {
        return;
    }

    i32 dx = x1 - x0;
    if (dx < 0) {
        dx = -dx;
    }

    i32 sx = (x0 < x1) ? 1 : -1;

    i32 dy = y1 - y0;
    if (dy > 0) {
        dy = -dy;
    }

    i32 sy = (y0 < y1) ? 1 : -1;
    i32 err = dx + dy;

    for (;;) {
        _put_pixel(fb, stride_pixels, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }

        i32 e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }

        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_circle(framebuffer_t *fb, i32 cx, i32 cy, u32 radius, pixel_t color) {
    if (!fb || !fb->pixels || !fb->width || !fb->height) {
        return;
    }

    size_t stride_pixels = _stride_pixels(fb);
    if (!stride_pixels) {
        return;
    }

    if (!radius) {
        _put_pixel(fb, stride_pixels, cx, cy, color);
        return;
    }

    i32 x = (i32)radius;
    i32 y = 0;
    i32 err = 1 - x;

    while (x >= y) {
        _put_pixel(fb, stride_pixels, cx + x, cy + y, color);
        _put_pixel(fb, stride_pixels, cx + y, cy + x, color);
        _put_pixel(fb, stride_pixels, cx - y, cy + x, color);
        _put_pixel(fb, stride_pixels, cx - x, cy + y, color);
        _put_pixel(fb, stride_pixels, cx - x, cy - y, color);
        _put_pixel(fb, stride_pixels, cx - y, cy - x, color);
        _put_pixel(fb, stride_pixels, cx + y, cy - x, color);
        _put_pixel(fb, stride_pixels, cx + x, cy - y, color);

        y++;
        if (err < 0) {
            err += 2 * y + 1;
            continue;
        }

        x--;
        err += 2 * (y - x) + 1;
    }
}

void draw_disk(framebuffer_t *fb, i32 cx, i32 cy, u32 radius, pixel_t color) {
    if (!fb || !fb->pixels || !fb->width || !fb->height) {
        return;
    }

    size_t stride_pixels = _stride_pixels(fb);
    if (!stride_pixels) {
        return;
    }

    if (!radius) {
        _put_pixel(fb, stride_pixels, cx, cy, color);
        return;
    }

    i32 x = (i32)radius;
    i32 y = 0;
    i32 err = 1 - x;

    while (x >= y) {
        _draw_hspan(fb, stride_pixels, cx - x, cx + x, cy + y, color);
        _draw_hspan(fb, stride_pixels, cx - x, cx + x, cy - y, color);

        if (x != y) {
            _draw_hspan(fb, stride_pixels, cx - y, cx + y, cy + x, color);
            _draw_hspan(fb, stride_pixels, cx - y, cx + y, cy - x, color);
        }

        y++;
        if (err < 0) {
            err += 2 * y + 1;
            continue;
        }

        x--;
        err += 2 * (y - x) + 1;
    }
}

void draw_text(framebuffer_t *fb, i32 x, i32 y, const char *text, pixel_t color) {
    if (!fb || !fb->pixels || !text || !text[0]) {
        return;
    }

    if (!_load_draw_font()) {
        return;
    }

    if (!font_cache.psf.glyphs || !font_cache.psf.width || !font_cache.psf.height || !font_cache.psf.row_bytes) {
        return;
    }

    size_t stride_pixels = _stride_pixels(fb);
    if (!stride_pixels) {
        return;
    }

    i32 pen_x = x;
    i32 pen_y = y;

    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            pen_x = x;
            pen_y += (i32)font_cache.cell_height;
            continue;
        }

        u32 idx = _glyph_index_for((u8)(*p));
        if (idx >= font_cache.psf.glyph_count) {
            idx = 0;
        }

        const u8 *glyph = font_cache.psf.glyphs + (size_t)idx * font_cache.psf.glyph_size;
        u32 glyph_x0 = font_cache.cell_src_x;
        u32 glyph_w = font_cache.psf.width;
        u32 glyph_h = font_cache.psf.height;

        if (glyph_x0 >= glyph_w) {
            glyph_x0 = 0;
        }

        u32 draw_w = font_cache.cell_width;
        u32 draw_h = font_cache.cell_height;
        u32 src_w = glyph_w - glyph_x0;

        if (draw_w > src_w) {
            draw_w = src_w;
        }

        if (draw_h > glyph_h) {
            draw_h = glyph_h;
        }

        for (u32 gy = 0; gy < draw_h; gy++) {
            const u8 *row_ptr = glyph + (size_t)gy * font_cache.psf.row_bytes;

            for (u32 gx = 0; gx < draw_w; gx++) {
                u32 src_x = glyph_x0 + gx;
                u8 bits = row_ptr[src_x / 8];
                u8 mask = (u8)(0x80 >> (src_x & 7));
                if (!(bits & mask)) {
                    continue;
                }

                _put_pixel(fb, stride_pixels, pen_x + (i32)gx, pen_y + (i32)gy, color);
            }
        }

        pen_x += (i32)font_cache.cell_width;
    }
}

void draw_triangle(framebuffer_t *fb, draw_point_t p0, draw_point_t p1, draw_point_t p2, pixel_t color) {
    if (!fb || !fb->pixels || !fb->width || !fb->height) {
        return;
    }

    size_t stride_pixels = _stride_pixels(fb);
    if (!stride_pixels) {
        return;
    }

    i32 min_x = min3(p0.x, p1.x, p2.x);
    i32 max_x = max3(p0.x, p1.x, p2.x);
    i32 min_y = min3(p0.y, p1.y, p2.y);
    i32 max_y = max3(p0.y, p1.y, p2.y);

    if (min_x < 0) {
        min_x = 0;
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_x >= (i32)fb->width) {
        max_x = (i32)fb->width - 1;
    }
    if (max_y >= (i32)fb->height) {
        max_y = (i32)fb->height - 1;
    }

    for (i32 y = min_y; y <= max_y; y++) {
        for (i32 x = min_x; x <= max_x; x++) {
            i64 w0 = edge(p0, p1, x, y);
            i64 w1 = edge(p1, p2, x, y);
            i64 w2 = edge(p2, p0, x, y);

            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))) {
                continue;
            }

            fb->pixels[(size_t)y * stride_pixels + (size_t)x] = color;
        }
    }
}

void draw_polygon(framebuffer_t *fb, const draw_point_t *points, size_t count, pixel_t color) {
    if (!fb || !fb->pixels || !points || count < 3) {
        return;
    }

    draw_point_t first = points[0];
    for (size_t i = 1; i + 1 < count; i++) {
        draw_triangle(fb, first, points[i], points[i + 1], color);
    }
}
