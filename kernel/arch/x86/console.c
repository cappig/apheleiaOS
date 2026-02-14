#include <arch/arch.h>
#include <string.h>
#include <sys/console.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/console.h>
#include <x86/vga.h>

static u8* x86_fb_base = NULL;
static u64 x86_fb_phys = 0;
static bool x86_fb_use_phys_window = false;

static bool _x86_console_probe(void* arch_boot_info, console_hw_desc_t* out) {
    if (!arch_boot_info || !out)
        return false;

    boot_info_t* info = arch_boot_info;
    memset(out, 0, sizeof(*out));

    if (info->video.mode == VIDEO_GRAPHICS && info->video.framebuffer && info->video.width &&
        info->video.height && info->video.bytes_per_pixel) {

        u32 pitch = info->video.bytes_per_line;
        if (!pitch)
            pitch = info->video.width * info->video.bytes_per_pixel;

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
            x86_fb_base = NULL;
            x86_fb_phys = info->video.framebuffer;
            x86_fb_use_phys_window = true;

            return true;
        }
#else
        u8* mapped = arch_phys_map(info->video.framebuffer, size);
        if (mapped) {
            out->mode = CONSOLE_FRAMEBUFFER;
            out->fb = mapped;
            out->fb_size = size;
            out->width = info->video.width;
            out->height = info->video.height;
            out->pitch = pitch;
            out->bytes_per_pixel = (u8)info->video.bytes_per_pixel;
            x86_fb_base = mapped;
            x86_fb_phys = info->video.framebuffer;
            x86_fb_use_phys_window = false;

            return true;
        }
#endif
    }

    if (info->video.mode == VIDEO_NONE)
        return false;

    size_t vga_size = VGA_WIDTH * VGA_HEIGHT * sizeof(u16);

    out->mode = CONSOLE_TEXT;
    out->fb_size = vga_size;
    out->width = VGA_WIDTH;
    out->height = VGA_HEIGHT;
    out->pitch = VGA_WIDTH * sizeof(u16);
    out->bytes_per_pixel = 2;

#if defined(__i386__)
    out->fb = (u8*)(uintptr_t)VGA_ADDR;
#else
    out->fb = arch_phys_map(VGA_ADDR, vga_size);
    if (!out->fb) {
        memset(out, 0, sizeof(*out));
        return false;
    }
#endif

    x86_fb_base = out->fb;
    x86_fb_phys = VGA_ADDR;
    x86_fb_use_phys_window = false;

    return true;
}

static u8* _x86_fb_map(size_t offset, size_t size) {
    if (!size)
        return NULL;

#if defined(__i386__)
    if (x86_fb_use_phys_window)
        return arch_phys_map(x86_fb_phys + (u64)offset, size);
#endif

    if (!x86_fb_base)
        return NULL;

    return x86_fb_base + offset;
}

static void _x86_fb_unmap(void* ptr, size_t size) {
#if defined(__i386__)
    if (x86_fb_use_phys_window)
        arch_phys_unmap(ptr, size);
#endif

    (void)ptr;
    (void)size;
}

static void _x86_text_cursor_set(size_t col, size_t row) {
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

static void _x86_text_put(u8* fb, size_t cols, size_t col, size_t row, u32 codepoint, u8 fg, u8 bg) {
    if (!fb)
        return;

    u16* text = (u16*)fb;
    text[row * cols + col] = _x86_text_cell(codepoint, fg, bg);
}

static void _x86_text_clear(u8* fb, size_t cols, size_t rows, u8 fg, u8 bg) {
    if (!fb)
        return;

    u16* text = (u16*)fb;
    u16 blank_cell = _x86_text_cell(' ', fg, bg);
    size_t count = cols * rows;

    for (size_t i = 0; i < count; i++)
        text[i] = blank_cell;
}

static void _x86_text_scroll_up(u8* fb, size_t cols, size_t rows, u8 fg, u8 bg) {
    if (!fb || !cols || !rows)
        return;

    u16* text = (u16*)fb;
    u16 blank_cell = _x86_text_cell(' ', fg, bg);

    memmove(text, text + cols, (rows - 1) * cols * sizeof(*text));

    size_t last_row = rows - 1;

    for (size_t col = 0; col < cols; col++)
        text[last_row * cols + col] = blank_cell;
}

static const console_backend_ops_t x86_console_ops = {
    .probe = _x86_console_probe,
    .fb_map = _x86_fb_map,
    .fb_unmap = _x86_fb_unmap,
    .text_cursor_set = _x86_text_cursor_set,
    .text_put = _x86_text_put,
    .text_clear = _x86_text_clear,
    .text_scroll_up = _x86_text_scroll_up,
};

void x86_console_backend_init(void) {
    console_backend_register(&x86_console_ops);
}
