#include <arch/arch.h>
#include <limits.h>
#include <string.h>
#include <sys/console.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/console.h>
#include <x86/vga.h>

typedef struct {
    u8 *fb;
    u64 phys;
    bool use_phys_window;
} x86_console_t;

static x86_console_t x86_console = { 0 };

static bool _x86_console_probe(void *arch_boot_info, console_hw_desc_t *out) {
    if (!arch_boot_info || !out) {
        return false;
    }

    boot_info_t *info = arch_boot_info;
    memset(out, 0, sizeof(*out));

    bool has_graphics_mode = info->video.mode == VIDEO_GRAPHICS;
    bool has_framebuffer = info->video.framebuffer != 0;
    bool has_dimensions = info->video.width && info->video.height;
    bool has_bpp = info->video.bytes_per_pixel != 0;

    if (has_graphics_mode && has_framebuffer && has_dimensions && has_bpp) {
        u32 pitch = info->video.bytes_per_line;

        if (!pitch) {
            pitch = info->video.width * info->video.bytes_per_pixel;
        }

        if (!pitch || (size_t)pitch > SIZE_MAX / (size_t)info->video.height) {
            return false;
        }

        size_t size = (size_t)pitch * (size_t)info->video.height;

#if defined(__i386__)
        if (size <= PHYS_WINDOW_SIZE_32) {
            out->mode = CONSOLE_FRAMEBUFFER;
            out->fb = NULL;
            out->fb_size = size;
            out->width = info->video.width;
            out->height = info->video.height;
            out->pitch = pitch;
            out->bytes_per_pixel = (u8)info->video.bytes_per_pixel;
            out->red_shift = info->video.red_shift;
            out->green_shift = info->video.green_shift;
            out->blue_shift = info->video.blue_shift;
            out->red_size = info->video.red_size;
            out->green_size = info->video.green_size;
            out->blue_size = info->video.blue_size;

            x86_console.fb = NULL;
            x86_console.phys = info->video.framebuffer;
            x86_console.use_phys_window = true;

            return true;
        }
#else
        u8 *mapped = arch_phys_map(info->video.framebuffer, size, PHYS_MAP_WC);
        if (mapped) {
            out->mode = CONSOLE_FRAMEBUFFER;
            out->fb = mapped;
            out->fb_size = size;
            out->width = info->video.width;
            out->height = info->video.height;
            out->pitch = pitch;
            out->bytes_per_pixel = (u8)info->video.bytes_per_pixel;
            out->red_shift = info->video.red_shift;
            out->green_shift = info->video.green_shift;
            out->blue_shift = info->video.blue_shift;
            out->red_size = info->video.red_size;
            out->green_size = info->video.green_size;
            out->blue_size = info->video.blue_size;

            x86_console.fb = mapped;
            x86_console.phys = info->video.framebuffer;
            x86_console.use_phys_window = false;

            return true;
        }
#endif
    }

    if (info->video.mode == VIDEO_NONE) {
        return false;
    }

    size_t vga_size = VGA_WIDTH * VGA_HEIGHT * sizeof(u16);

    out->mode = CONSOLE_TEXT;
    out->fb_size = vga_size;
    out->width = VGA_WIDTH;
    out->height = VGA_HEIGHT;
    out->pitch = VGA_WIDTH * sizeof(u16);
    out->bytes_per_pixel = 2;

#if defined(__i386__)
    out->fb = (u8 *)(uintptr_t)VGA_ADDR;
#else
    out->fb = arch_phys_map(VGA_ADDR, vga_size, 0);
    if (!out->fb) {
        memset(out, 0, sizeof(*out));
        return false;
    }
#endif

    x86_console.fb = out->fb;
    x86_console.phys = VGA_ADDR;
    x86_console.use_phys_window = false;

    return true;
}

static u8 *_x86_fb_map(size_t offset, size_t size) {
    if (!size) {
        return NULL;
    }

#if defined(__i386__)
    if (x86_console.use_phys_window) {
        return arch_phys_map(x86_console.phys + (u64)offset, size, PHYS_MAP_WC);
    }
#endif

    if (!x86_console.fb) {
        return NULL;
    }

    return x86_console.fb + offset;
}

static void _x86_fb_unmap(void *ptr, size_t size) {
#if defined(__i386__)
    if (x86_console.use_phys_window) {
        arch_phys_unmap(ptr, size);
    }
#endif

    (void)ptr;
    (void)size;
}

static void _text_cursor_set(size_t col, size_t row) {
    u16 pos = (u16)(row * VGA_WIDTH + col);

    outb(0x3d4, 0x0f);
    outb(0x3d5, (u8)(pos & 0xff));
    outb(0x3d4, 0x0e);
    outb(0x3d5, (u8)((pos >> 8) & 0xff));
}

static u16 _x86_text_cell(u32 codepoint, u8 fg, u8 bg) {
    u8 ch = codepoint > 0xff ? (u8)'?' : (u8)codepoint;
    u8 attr = (u8)((bg << 4) | (fg & 0x0f));
    return ((u16)attr << 8) | ch;
}

static void _x86_text_put(const console_text_cell_t *cell) {
    if (!cell || !cell->fb) {
        return;
    }

    u16 *text = (u16 *)cell->fb;
    text[cell->row * cell->cols + cell->col] = _x86_text_cell(cell->codepoint, cell->fg, cell->bg);
}

static void _x86_text_clear(const console_text_region_t *region) {
    if (!region || !region->fb) {
        return;
    }

    u16 *text = (u16 *)region->fb;
    u16 blank_cell = _x86_text_cell(' ', region->fg, region->bg);
    size_t count = region->cols * region->rows;

    for (size_t i = 0; i < count; i++) {
        text[i] = blank_cell;
    }
}

static void _x86_text_scroll_up(const console_text_region_t *region) {
    if (!region || !region->fb || !region->cols || !region->rows) {
        return;
    }

    u16 *text = (u16 *)region->fb;
    u16 blank_cell = _x86_text_cell(' ', region->fg, region->bg);
    size_t last_row = region->rows - 1;

    memmove(text, text + region->cols, last_row * region->cols * sizeof(*text));

    for (size_t col = 0; col < region->cols; col++) {
        text[last_row * region->cols + col] = blank_cell;
    }
}

static const console_backend_ops_t x86_console_ops = {
    .probe = _x86_console_probe,
    .fb_map = _x86_fb_map,
    .fb_unmap = _x86_fb_unmap,
    .text_cursor_set = _text_cursor_set,
    .text_put = _x86_text_put,
    .text_clear = _x86_text_clear,
    .text_scroll_up = _x86_text_scroll_up,
};

void x86_console_init(void) {
    console_set_backend(&x86_console_ops);
}
