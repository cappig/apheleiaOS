#include "draw.h"

#include <ctype.h>
#include <limits.h>
#include <psf.h>
#include <stdlib.h>
#include <string.h>
#include <term/glyph.h>
#include <user/kv.h>

#define DRAW_FONT_BUF_SIZE (256 * 1024)
#define DRAW_FONT_PATH_DEFAULT "/etc/font.psf"
#define DRAW_WM_CONFIG_PATH "/etc/wm.conf"
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

static u8 draw_font_buf[DRAW_FONT_BUF_SIZE];
static psf_font_t draw_font = {0};
static draw_font_map_entry_t *draw_font_map = NULL;
static size_t draw_font_map_count = 0;
static size_t draw_font_map_capacity = 0;
static u32 draw_font_cell_width = 8;
static u32 draw_font_cell_height = 16;
static u32 draw_font_cell_src_x = 0;
static bool draw_font_load_attempted = false;
static bool draw_font_loaded = false;
static char draw_font_path[DRAW_FONT_PATH_MAX] = DRAW_FONT_PATH_DEFAULT;
static bool draw_font_path_initialized = false;

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
    return (i64)(px - a.x) * (i64)(b.y - a.y) -
           (i64)(py - a.y) * (i64)(b.x - a.x);
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

static bool _glyph_pixel_on(
    const u8 *glyph,
    u32 row_bytes,
    u32 x,
    u32 y
) {
    const u8 *row_ptr = glyph + (size_t)y * row_bytes;
    u8 bits = row_ptr[x / 8];
    u8 mask = (u8)(0x80 >> (x & 7));
    return (bits & mask) != 0;
}

static void _clear_font_map(void) {
    if (!draw_font_map) {
        return;
    }

    free(draw_font_map);
    draw_font_map = NULL;
    draw_font_map_count = 0;
    draw_font_map_capacity = 0;
}

static void _reset_draw_font_state(void) {
    memset(&draw_font, 0, sizeof(draw_font));
    _clear_font_map();
    draw_font_cell_src_x = 0;
    draw_font_cell_width = 8U;
    draw_font_cell_height = 16U;
    draw_font_load_attempted = false;
    draw_font_loaded = false;
}

static bool _reserve_font_map(size_t needed) {
    if (draw_font_map_capacity >= needed) {
        return true;
    }

    size_t new_cap = draw_font_map_capacity ? draw_font_map_capacity * 2 : 128;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    draw_font_map_entry_t *next = malloc(new_cap * sizeof(*next));
    if (!next) {
        return false;
    }

    if (draw_font_map && draw_font_map_count) {
        memcpy(next, draw_font_map, draw_font_map_count * sizeof(*next));
    }

    free(draw_font_map);
    draw_font_map = next;
    draw_font_map_capacity = new_cap;
    return true;
}

static bool _push_font_map(u32 codepoint, u32 glyph) {
    if (!_reserve_font_map(draw_font_map_count + 1)) {
        return false;
    }

    draw_font_map[draw_font_map_count++] = (draw_font_map_entry_t){
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
    for (size_t i = 1; i < draw_font_map_count; i++) {
        draw_font_map_entry_t current = draw_font_map[i];
        size_t j = i;

        while (j > 0 && draw_font_map[j - 1].codepoint > current.codepoint) {
            draw_font_map[j] = draw_font_map[j - 1];
            j--;
        }

        draw_font_map[j] = current;
    }
}

static bool _find_mapped_glyph(u32 codepoint, u32 *glyph_out) {
    if (!glyph_out || !draw_font_map || !draw_font_map_count) {
        return false;
    }

    size_t lo = 0;
    size_t hi = draw_font_map_count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const draw_font_map_entry_t *entry = &draw_font_map[mid];

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
        if (glyph < draw_font.glyph_count) {
            return glyph;
        }
    }

    if (!draw_font_map_count && codepoint < draw_font.glyph_count) {
        return codepoint;
    }

    if (_find_mapped_glyph((u32)'?', &glyph)) {
        if (glyph < draw_font.glyph_count) {
            return glyph;
        }
    }

    if (!draw_font_map_count && (u32)'?' < draw_font.glyph_count) {
        return (u32)'?';
    }

    return 0;
}

static bool _glyph_bounds_for_index(u32 glyph_idx, u32 *left_out, u32 *right_out) {
    if (
        glyph_idx >= draw_font.glyph_count ||
        !left_out ||
        !right_out
    ) {
        return false;
    }

    const u8 *glyph =
        draw_font.glyphs + (size_t)glyph_idx * draw_font.glyph_size;
    u32 left = draw_font.width;
    u32 right = 0;
    bool any = false;

    for (u32 gy = 0; gy < draw_font.height; gy++) {
        for (u32 gx = 0; gx < draw_font.width; gx++) {
            if (!_glyph_pixel_on(glyph, draw_font.row_bytes, gx, gy)) {
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
    draw_font_cell_src_x = 0;
    draw_font_cell_width = draw_font.width ? draw_font.width : 8U;
    draw_font_cell_height = draw_font.height ? draw_font.height : 16U;

    if (
        !draw_font.glyphs ||
        !draw_font.width ||
        !draw_font.height ||
        !draw_font.row_bytes
    ) {
        return;
    }

    u32 min_left = draw_font.width;
    u32 max_right = 0;
    bool have_bounds = false;

    // Base spacing on printable ASCII to keep text layout stable.
    for (u32 cp = 32; cp < 127; cp++) {
        u32 glyph = 0;
        if (draw_font_map_count) {
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
    u32 max_advance = draw_font.width - min_left;

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

    draw_font_cell_src_x = min_left;
    draw_font_cell_width = advance;
}

static void _resolve_draw_font_path(void) {
    if (draw_font_path_initialized) {
        return;
    }

    draw_font_path_initialized = true;

    char cfg_text[DRAW_WM_CONFIG_BUF_SIZE];
    if (
        kv_read_file(DRAW_WM_CONFIG_PATH, cfg_text, sizeof(cfg_text)) <= 0
    ) {
        return;
    }

    char configured[DRAW_FONT_PATH_MAX] = {0};
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
    if (len >= sizeof(draw_font_path)) {
        return;
    }

    memcpy(draw_font_path, start, len + 1);
}

static bool _load_draw_font(void) {
    if (draw_font_load_attempted) {
        return draw_font_loaded;
    }

    _resolve_draw_font_path();
    draw_font_load_attempted = true;
    _clear_font_map();

    if (psf_load_file(
            draw_font_path,
            draw_font_buf,
            sizeof(draw_font_buf),
            &draw_font
        )) {
        if (
            !psf_iter_unicode_mappings(
                &draw_font, _push_font_map_iter, NULL
            )
        ) {
            memset(&draw_font, 0, sizeof(draw_font));
            _clear_font_map();
            draw_font_loaded = false;
            return false;
        }

        _sort_font_map();
        _derive_font_metrics();
        draw_font_loaded = true;
        return true;
    }

    memset(&draw_font, 0, sizeof(draw_font));
    _clear_font_map();
    draw_font_cell_src_x = 0;
    draw_font_cell_width = 8U;
    draw_font_cell_height = 16U;
    draw_font_loaded = false;
    return false;
}

bool draw_set_font_path(const char *path) {
    if (!path || !path[0]) {
        return false;
    }

    size_t len = strlen(path);
    if (len >= sizeof(draw_font_path)) {
        return false;
    }

    if (!strcmp(draw_font_path, path)) {
        draw_font_path_initialized = true;
        return true;
    }

    memcpy(draw_font_path, path, len + 1);
    draw_font_path_initialized = true;
    _reset_draw_font_state();
    return true;
}

const char *draw_get_font_path(void) {
    _resolve_draw_font_path();
    if (!draw_font_path[0]) {
        return DRAW_FONT_PATH_DEFAULT;
    }
    return draw_font_path;
}

u32 draw_font_width(void) {
    if (_load_draw_font() && draw_font_cell_width) {
        return draw_font_cell_width;
    }

    return 8U;
}

u32 draw_font_height(void) {
    if (_load_draw_font() && draw_font_cell_height) {
        return draw_font_cell_height;
    }

    return 16U;
}

static inline void
_put_pixel(framebuffer_t *fb, size_t stride_pixels, i32 x, i32 y, pixel_t color) {
    if (!fb || !fb->pixels || x < 0 || y < 0) {
        return;
    }

    if (x >= (i32)fb->width || y >= (i32)fb->height) {
        return;
    }

    fb->pixels[(size_t)y * stride_pixels + (size_t)x] = color;
}

static inline void _draw_hspan(
    framebuffer_t *fb,
    size_t stride_pixels,
    i32 x0,
    i32 x1,
    i32 y,
    pixel_t color
) {
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

void draw_rect(
    framebuffer_t *fb,
    i32 x,
    i32 y,
    u32 width,
    u32 height,
    pixel_t color
) {
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
    bool byte_fill =
        (b0 == (u8)(color >> 8)) && (b0 == (u8)(color >> 16)) && (b0 == (u8)(color >> 24));

    if (byte_fill) {
        for (i32 row = y0; row < y1; row++) {
            memset(
                fb->pixels + (size_t)row * stride_pixels + (size_t)x0,
                (int)b0,
                span_bytes
            );
        }
        return;
    }

    // Fill the first row manually, then memcpy to subsequent rows
    pixel_t *first_row = fb->pixels + (size_t)y0 * stride_pixels + (size_t)x0;

    for (size_t i = 0; i < span; i++) {
        first_row[i] = color;
    }

    for (i32 row = y0 + 1; row < y1; row++) {
        memcpy(
            fb->pixels + (size_t)row * stride_pixels + (size_t)x0,
            first_row,
            span_bytes
        );
    }
}

void draw_line(
    framebuffer_t *fb,
    i32 x0,
    i32 y0,
    i32 x1,
    i32 y1,
    pixel_t color
) {
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

void draw_circle(
    framebuffer_t *fb,
    i32 cx,
    i32 cy,
    u32 radius,
    pixel_t color
) {
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

void draw_disk(
    framebuffer_t *fb,
    i32 cx,
    i32 cy,
    u32 radius,
    pixel_t color
) {
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

void draw_text(
    framebuffer_t *fb,
    i32 x,
    i32 y,
    const char *text,
    pixel_t color
) {
    if (!fb || !fb->pixels || !text || !text[0]) {
        return;
    }

    if (!_load_draw_font()) {
        return;
    }

    if (!draw_font.glyphs || !draw_font.width || !draw_font.height || !draw_font.row_bytes) {
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
            pen_y += (i32)draw_font_cell_height;
            continue;
        }

        u32 idx = _glyph_index_for((u8)(*p));
        if (idx >= draw_font.glyph_count) {
            idx = 0;
        }

        const u8 *glyph = draw_font.glyphs + (size_t)idx * draw_font.glyph_size;
        u32 glyph_x0 = draw_font_cell_src_x;
        u32 glyph_w = draw_font.width;
        u32 glyph_h = draw_font.height;

        if (glyph_x0 >= glyph_w) {
            glyph_x0 = 0;
        }

        u32 draw_w = draw_font_cell_width;
        u32 draw_h = draw_font_cell_height;
        u32 src_w = glyph_w - glyph_x0;

        if (draw_w > src_w) {
            draw_w = src_w;
        }

        if (draw_h > glyph_h) {
            draw_h = glyph_h;
        }

        for (u32 gy = 0; gy < draw_h; gy++) {
            const u8 *row_ptr = glyph + (size_t)gy * draw_font.row_bytes;

            for (u32 gx = 0; gx < draw_w; gx++) {
                u32 src_x = glyph_x0 + gx;
                u8 bits = row_ptr[src_x / 8];
                u8 mask = (u8)(0x80 >> (src_x & 7));
                if (!(bits & mask)) {
                    continue;
                }

                _put_pixel(
                    fb,
                    stride_pixels,
                    pen_x + (i32)gx,
                    pen_y + (i32)gy,
                    color
                );
            }
        }

        pen_x += (i32)draw_font_cell_width;
    }
}

void draw_triangle(
    framebuffer_t *fb,
    draw_point_t p0,
    draw_point_t p1,
    draw_point_t p2,
    pixel_t color
) {
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

void draw_polygon(
    framebuffer_t *fb,
    const draw_point_t *points,
    size_t count,
    pixel_t color
) {
    if (!fb || !fb->pixels || !points || count < 3) {
        return;
    }

    draw_point_t first = points[0];
    for (size_t i = 1; i + 1 < count; i++) {
        draw_triangle(fb, first, points[i], points[i + 1], color);
    }
}
