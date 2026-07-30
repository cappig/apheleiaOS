#include <ctype.h>
#include <errno.h>
#include <gui/input.h>
#include <input/kbd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ui.h>
#include <unistd.h>

#include "doomgeneric.h"
#include "doomkeys.h"

#define DG_KEYQUEUE_SIZE 64

typedef struct {
    window_t window;
    bool window_ready;
    bool window_closed;
    bool close_key_sent;

    unsigned short key_queue[DG_KEYQUEUE_SIZE];
    size_t key_read;
    size_t key_write;

    struct timespec ticks_start;
    uint32_t last_ticks_ms;
} doom_port_t;

static doom_port_t doom = { 0 };

static bool dg_has_iwad_arg(int argc, char **argv) {
    if (!argv) {
        return false;
    }

    for (int i = 1; i < argc; i++) {
        if (!argv[i]) {
            continue;
        }

        if (!strcmp(argv[i], "-iwad")) {
            return true;
        }
    }

    return false;
}

static void dg_key_push(int pressed, unsigned char key) {
    size_t next = (doom.key_write + 1) % DG_KEYQUEUE_SIZE;

    if (next == doom.key_read) {
        doom.key_read = (doom.key_read + 1) % DG_KEYQUEUE_SIZE;
    }

    doom.key_queue[doom.key_write] = (unsigned short)(((pressed ? 1U : 0U) << 8) | key);
    doom.key_write = next;
}

static int dg_key_pop(int *pressed, unsigned char *key) {
    if (doom.key_read == doom.key_write || !pressed || !key) {
        return 0;
    }

    unsigned short entry = doom.key_queue[doom.key_read];
    doom.key_read = (doom.key_read + 1) % DG_KEYQUEUE_SIZE;

    *pressed = (entry >> 8) & 1U;
    *key = (unsigned char)(entry & 0xffU);
    return 1;
}

static void mark_window_closed(void) {
    doom.window_closed = true;

    if (!doom.close_key_sent) {
        dg_key_push(1, KEY_ESCAPE);
        doom.close_key_sent = true;
    }
}

typedef struct {
    uint32_t code;
    unsigned char normal;
    unsigned char shifted;
} ascii_key_t;

typedef struct {
    uint32_t code;
    unsigned char doom;
} doom_key_t;

static unsigned char lookup_ascii_key(uint32_t keycode, bool shifted) {
    static const ascii_key_t keys[] = {
        { KBD_MINUS, '-', '_' },         { KBD_EQUALS, '=', '+' },      { KBD_LEFT_BRACKET, '[', '{' },
        { KBD_RIGHT_BRACKET, ']', '}' }, { KBD_BACKSLASH, '\\', '|' },  { KBD_SEMICOLON, ';', ':' },
        { KBD_QUOTE, '\'', '"' },        { KBD_BACKTICK, '`', '~' },    { KBD_COMMA, ',', '<' },
        { KBD_DOT, '.', '>' },           { KBD_SLASH, '/', '?' },       { KBD_SPACE, ' ', ' ' },
        { KBD_KP_DIVIDE, '/', '/' },     { KBD_KP_MULTIPLY, '*', '*' }, { KBD_KP_PLUS, '+', '+' },
        { KBD_KP_PERIOD, '.', '.' },
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (keys[i].code == keycode) {
            return shifted ? keys[i].shifted : keys[i].normal;
        }
    }

    return 0;
}

static unsigned char dg_ascii_key(uint32_t keycode, bool shifted) {
    if (keycode >= KBD_A && keycode <= KBD_Z) {
        return (unsigned char)('a' + (keycode - KBD_A));
    }

    if (keycode >= KBD_0 && keycode <= KBD_9) {
        static const char normal[] = "0123456789";
        static const char shifted_map[] = ")!@#$%^&*(";
        uint32_t idx = keycode - KBD_0;
        return (unsigned char)(shifted ? shifted_map[idx] : normal[idx]);
    }

    if (keycode >= KBD_KP_0 && keycode <= KBD_KP_9) {
        return (unsigned char)('0' + (keycode - KBD_KP_0));
    }

    return lookup_ascii_key(keycode, shifted);
}

static unsigned char lookup_doom_key(uint32_t keycode) {
    static const doom_key_t keys[] = {
        { KBD_ENTER, KEY_ENTER },
        { KBD_KP_ENTER, KEY_ENTER },
        { KBD_ESCAPE, KEY_ESCAPE },
        { KBD_LEFT, KEY_LEFTARROW },
        { KBD_RIGHT, KEY_RIGHTARROW },
        { KBD_UP, KEY_UPARROW },
        { KBD_DOWN, KEY_DOWNARROW },
        { KBD_LEFT_CTRL, KEY_FIRE },
        { KBD_RIGHT_CTRL, KEY_FIRE },
        { KBD_SPACE, KEY_USE },
        { KBD_LEFT_SHIFT, KEY_RSHIFT },
        { KBD_RIGHT_SHIFT, KEY_RSHIFT },
        { KBD_LEFT_ALT, KEY_LALT },
        { KBD_RIGHT_ALT, KEY_LALT },
        { KBD_TAB, KEY_TAB },
        { KBD_BACKSPACE, KEY_BACKSPACE },
        { KBD_DELETE, KEY_DEL },
        { KBD_INSERT, KEY_INS },
        { KBD_HOME, KEY_HOME },
        { KBD_END, KEY_END },
        { KBD_PAGEUP, KEY_PGUP },
        { KBD_PAGEDOWN, KEY_PGDN },
        { KBD_F1, KEY_F1 },
        { KBD_F2, KEY_F2 },
        { KBD_F3, KEY_F3 },
        { KBD_F4, KEY_F4 },
        { KBD_F5, KEY_F5 },
        { KBD_F6, KEY_F6 },
        { KBD_F7, KEY_F7 },
        { KBD_F8, KEY_F8 },
        { KBD_F9, KEY_F9 },
        { KBD_F10, KEY_F10 },
        { KBD_F11, KEY_F11 },
        { KBD_F12, KEY_F12 },
        { KBD_EQUALS, KEY_EQUALS },
        { KBD_MINUS, KEY_MINUS },
        { KBD_KP_MINUS, KEY_MINUS },
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (keys[i].code == keycode) {
            return keys[i].doom;
        }
    }

    return 0;
}

static unsigned char dg_convert_key(const ws_input_event_t *event) {
    if (!event) {
        return 0;
    }

    unsigned char doom_key = lookup_doom_key(event->keycode);
    if (doom_key) {
        return doom_key;
    }

    bool shifted = (event->modifiers & INPUT_MOD_SHIFT) != 0U;
    char ascii = (char)dg_ascii_key(event->keycode, shifted);
    if (!ascii) {
        return 0;
    }

    return (unsigned char)tolower((unsigned char)ascii);
}

static void dg_pump_events(void) {
    if (!doom.window_ready || doom.window_closed) {
        return;
    }

    for (;;) {
        ws_input_event_t events[16];
        ssize_t n = window_events(&doom.window, events, 16);

        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                break;
            }

            if (errno == ENOENT) {
                mark_window_closed();
            }
            break;
        }

        if (n == 0) {
            break;
        }

        size_t count = (size_t)n / sizeof(events[0]);
        if (!count) {
            break;
        }

        for (size_t i = 0; i < count; i++) {
            ws_input_event_t *event = &events[i];

            if (event->type != INPUT_EVENT_KEY) {
                continue;
            }

            unsigned char doom_key = dg_convert_key(event);
            if (!doom_key) {
                continue;
            }

            dg_key_push(event->action ? 1 : 0, doom_key);
        }

        if (count < 16) {
            break;
        }
    }
}

static void dg_blit_frame(void) {
    framebuffer_t *fb = window_buffer(&doom.window);
    if (!fb || !fb->pixels) {
        return;
    }

    size_t pixels = fb->pixel_count;
    for (size_t i = 0; i < pixels; i++) {
        fb->pixels[i] = 0;
    }

    uint32_t copy_w = DOOMGENERIC_RESX;
    uint32_t copy_h = DOOMGENERIC_RESY;

    if (copy_w > fb->width) {
        copy_w = fb->width;
    }
    if (copy_h > fb->height) {
        copy_h = fb->height;
    }

    uint32_t dst_x = (fb->width > copy_w) ? (fb->width - copy_w) / 2U : 0U;
    uint32_t dst_y = (fb->height > copy_h) ? (fb->height - copy_h) / 2U : 0U;
    uint32_t src_x = (DOOMGENERIC_RESX > copy_w) ? (DOOMGENERIC_RESX - copy_w) / 2U : 0U;
    uint32_t src_y = (DOOMGENERIC_RESY > copy_h) ? (DOOMGENERIC_RESY - copy_h) / 2U : 0U;

    const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;
    for (uint32_t y = 0; y < copy_h; y++) {
        const uint32_t *src_row = src + (size_t)(src_y + y) * DOOMGENERIC_RESX + src_x;
        uint32_t *dst_row = fb->pixels + (size_t)(dst_y + y) * fb->width + dst_x;

        memcpy(dst_row, src_row, (size_t)copy_w * sizeof(uint32_t));
    }

    if (window_flush(&doom.window) < 0) {
        if (errno == ENOENT) {
            mark_window_closed();
        }
    }
}

void DG_Init(void) {
    memset(doom.key_queue, 0, sizeof(doom.key_queue));
    doom.key_read = 0;
    doom.key_write = 0;
    doom.window_closed = false;
    doom.close_key_sent = false;

    ws_hints_t hints = {
        .min_width = 320,
        .min_height = 200,
    };
    if (window_init(&doom.window, DOOMGENERIC_RESX, DOOMGENERIC_RESY, "doom", &hints)) {
        fprintf(stderr, "doom: failed to create window (%d: %s)\n", errno, strerror(errno));
        exit(1);
    }

    framebuffer_t *fb = window_buffer(&doom.window);
    if (!fb || !fb->pixels) {
        fprintf(stderr, "doom: failed to acquire window framebuffer\n");
        window_deinit(&doom.window);
        exit(1);
    }

    doom.window_ready = true;

    if (clock_gettime(CLOCK_MONOTONIC, &doom.ticks_start) < 0) {
        doom.ticks_start.tv_sec = 0;
        doom.ticks_start.tv_nsec = 0;
    }

    doom.last_ticks_ms = 0;
}

void DG_DrawFrame(void) {
    dg_pump_events();

    if (doom.window_closed) {
        _Exit(0);
    }

    dg_blit_frame();

    if (doom.window_closed) {
        _Exit(0);
    }
}

void DG_SleepMs(uint32_t ms) {
    if (!ms) {
        return;
    }

    usleep(ms * 1000U);
}

uint32_t DG_GetTicksMs(void) {
    struct timespec now = { 0 };
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return doom.last_ticks_ms;
    }

    uint64_t sec = 0;
    int64_t nsec = 0;

    if (now.tv_sec >= doom.ticks_start.tv_sec) {
        sec = (uint64_t)(now.tv_sec - doom.ticks_start.tv_sec);
    }

    nsec = now.tv_nsec - doom.ticks_start.tv_nsec;
    if (nsec < 0) {
        if (sec > 0) {
            sec--;
        }
        nsec += 1000000000LL;
    }

    uint64_t ms = sec * 1000ULL + (uint64_t)(nsec / 1000000LL);
    uint32_t ticks_ms = (uint32_t)ms;

    if (ticks_ms < doom.last_ticks_ms) {
        ticks_ms = doom.last_ticks_ms;
    } else {
        doom.last_ticks_ms = ticks_ms;
    }

    return ticks_ms;
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    dg_pump_events();

    if (doom.window_closed && !doom.close_key_sent) {
        dg_key_push(1, KEY_ESCAPE);
        doom.close_key_sent = true;
    }

    return dg_key_pop(pressed, doomKey);
}

void DG_SetWindowTitle(const char *title) {
    if (!doom.window_ready || doom.window_closed) {
        return;
    }

    if (!title || !title[0]) {
        title = "doom";
    }

    if (window_set_title(&doom.window, title) < 0 && errno == ENOENT) {
        mark_window_closed();
    }
}

int main(int argc, char **argv) {
    static const char *default_iwad = "/home/user/doom1.wad";
    char **argv_with_iwad = NULL;
    int argc_with_iwad = argc;

    puts("DOOM (C) id Software.");
    puts("DoomGeneric by ozkl and contributors.");

    if (!dg_has_iwad_arg(argc, argv) && access(default_iwad, R_OK) == 0) {
        argv_with_iwad = malloc((size_t)(argc + 3) * sizeof(char *));
        if (argv_with_iwad) {
            for (int i = 0; i < argc; i++) {
                argv_with_iwad[i] = argv[i];
            }

            argv_with_iwad[argc] = "-iwad";
            argv_with_iwad[argc + 1] = (char *)default_iwad;
            argv_with_iwad[argc + 2] = NULL;
            argc_with_iwad = argc + 2;
        }
    }

    doomgeneric_Create(argc_with_iwad, argv_with_iwad ? argv_with_iwad : argv);

    for (;;) {
        doomgeneric_Tick();
    }

    return 0;
}
