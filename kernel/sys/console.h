#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/font.h>
#include <sys/types.h>

typedef enum {
    CONSOLE_DISABLED = 0,
    CONSOLE_TEXT = 1,
    CONSOLE_FRAMEBUFFER = 2,
} console_mode_t;

typedef struct {
    console_mode_t mode;
    u8 *fb;
    size_t fb_size;
    u32 width;
    u32 height;
    u32 pitch;
    u8 bytes_per_pixel;
    u8 red_shift;
    u8 green_shift;
    u8 blue_shift;
    u8 red_size;
    u8 green_size;
    u8 blue_size;
} console_hw_desc_t;

typedef struct console_backend_ops {
    bool (*probe)(void *arch_boot_info, console_hw_desc_t *out);
    u8 *(*fb_map)(size_t offset, size_t size);
    void (*fb_unmap)(void *ptr, size_t size);
    void (*text_cursor_set)(size_t col, size_t row);
    void (*text_put)(u8 *fb, size_t cols, size_t col, size_t row, u32 codepoint, u8 fg, u8 bg);
    void (*text_clear)(u8 *fb, size_t cols, size_t rows, u8 fg, u8 bg);
    void (*text_scroll_up)(u8 *fb, size_t cols, size_t rows, u8 fg, u8 bg);
} console_backend_ops_t;

void console_init(void *arch_boot_info);
void console_set_font(const font_t *font);

ssize_t console_read(void *buf, size_t len);
ssize_t console_write(const void *buf, size_t len);
ssize_t console_write_screen(size_t screen, const void *buf, size_t len);

bool console_set_active(size_t screen);
bool console_get_size(size_t *cols, size_t *rows);

int console_fb_acquire(pid_t pid, size_t screen);
int console_fb_release(pid_t pid);
ssize_t console_fb_owner_screen(void);

void console_panic(void);

void console_backend_register(const console_backend_ops_t *ops);
