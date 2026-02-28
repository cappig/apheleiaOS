#include "doomgeneric.h"
#include "doomkeys.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gui/input.h>
#include <input/kbd.h>
#include <ui.h>

#define DG_KEYQUEUE_SIZE 64

static window_t s_window = {0};
static bool s_window_ready = false;
static bool s_window_closed = false;
static bool s_close_key_sent = false;

static unsigned short s_key_queue[DG_KEYQUEUE_SIZE];
static size_t s_key_read = 0;
static size_t s_key_write = 0;

static struct timespec s_ticks_start = {0};

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
    size_t next = (s_key_write + 1) % DG_KEYQUEUE_SIZE;

    if (next == s_key_read) {
        s_key_read = (s_key_read + 1) % DG_KEYQUEUE_SIZE;
    }

    s_key_queue[s_key_write] = (unsigned short)(((pressed ? 1U : 0U) << 8) | key);
    s_key_write = next;
}

static int dg_key_pop(int *pressed, unsigned char *key) {
    if (s_key_read == s_key_write || !pressed || !key) {
        return 0;
    }

    unsigned short entry = s_key_queue[s_key_read];
    s_key_read = (s_key_read + 1) % DG_KEYQUEUE_SIZE;

    *pressed = (entry >> 8) & 1U;
    *key = (unsigned char)(entry & 0xffU);
    return 1;
}

static void dg_mark_window_closed(void) {
    s_window_closed = true;

    if (!s_close_key_sent) {
        dg_key_push(1, KEY_ESCAPE);
        s_close_key_sent = true;
    }
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

    switch (keycode) {
    case KBD_MINUS:
        return (unsigned char)(shifted ? '_' : '-');
    case KBD_EQUALS:
        return (unsigned char)(shifted ? '+' : '=');
    case KBD_LEFT_BRACKET:
        return (unsigned char)(shifted ? '{' : '[');
    case KBD_RIGHT_BRACKET:
        return (unsigned char)(shifted ? '}' : ']');
    case KBD_BACKSLASH:
        return (unsigned char)(shifted ? '|' : '\\');
    case KBD_SEMICOLON:
        return (unsigned char)(shifted ? ':' : ';');
    case KBD_QUOTE:
        return (unsigned char)(shifted ? '"' : '\'');
    case KBD_BACKTICK:
        return (unsigned char)(shifted ? '~' : '`');
    case KBD_COMMA:
        return (unsigned char)(shifted ? '<' : ',');
    case KBD_DOT:
        return (unsigned char)(shifted ? '>' : '.');
    case KBD_SLASH:
        return (unsigned char)(shifted ? '?' : '/');
    case KBD_SPACE:
        return ' ';
    case KBD_KP_0:
        return '0';
    case KBD_KP_1:
        return '1';
    case KBD_KP_2:
        return '2';
    case KBD_KP_3:
        return '3';
    case KBD_KP_4:
        return '4';
    case KBD_KP_5:
        return '5';
    case KBD_KP_6:
        return '6';
    case KBD_KP_7:
        return '7';
    case KBD_KP_8:
        return '8';
    case KBD_KP_9:
        return '9';
    case KBD_KP_DIVIDE:
        return '/';
    case KBD_KP_MULTIPLY:
        return '*';
    case KBD_KP_PLUS:
        return '+';
    case KBD_KP_PERIOD:
        return '.';
    default:
        return 0;
    }
}

static unsigned char dg_convert_key(const ws_input_event_t *event) {
    if (!event) {
        return 0;
    }

    switch (event->keycode) {
    case KBD_ENTER:
    case KBD_KP_ENTER:
        return KEY_ENTER;
    case KBD_ESCAPE:
        return KEY_ESCAPE;
    case KBD_LEFT:
        return KEY_LEFTARROW;
    case KBD_RIGHT:
        return KEY_RIGHTARROW;
    case KBD_UP:
        return KEY_UPARROW;
    case KBD_DOWN:
        return KEY_DOWNARROW;
    case KBD_LEFT_CTRL:
    case KBD_RIGHT_CTRL:
        return KEY_FIRE;
    case KBD_SPACE:
        return KEY_USE;
    case KBD_LEFT_SHIFT:
    case KBD_RIGHT_SHIFT:
        return KEY_RSHIFT;
    case KBD_LEFT_ALT:
    case KBD_RIGHT_ALT:
        return KEY_LALT;
    case KBD_TAB:
        return KEY_TAB;
    case KBD_BACKSPACE:
        return KEY_BACKSPACE;
    case KBD_DELETE:
        return KEY_DEL;
    case KBD_INSERT:
        return KEY_INS;
    case KBD_HOME:
        return KEY_HOME;
    case KBD_END:
        return KEY_END;
    case KBD_PAGEUP:
        return KEY_PGUP;
    case KBD_PAGEDOWN:
        return KEY_PGDN;
    case KBD_F1:
        return KEY_F1;
    case KBD_F2:
        return KEY_F2;
    case KBD_F3:
        return KEY_F3;
    case KBD_F4:
        return KEY_F4;
    case KBD_F5:
        return KEY_F5;
    case KBD_F6:
        return KEY_F6;
    case KBD_F7:
        return KEY_F7;
    case KBD_F8:
        return KEY_F8;
    case KBD_F9:
        return KEY_F9;
    case KBD_F10:
        return KEY_F10;
    case KBD_F11:
        return KEY_F11;
    case KBD_F12:
        return KEY_F12;
    case KBD_EQUALS:
        return KEY_EQUALS;
    case KBD_MINUS:
    case KBD_KP_MINUS:
        return KEY_MINUS;
    default:
        break;
    }

    bool shifted = (event->modifiers & INPUT_MOD_SHIFT) != 0U;
    char ascii = (char)dg_ascii_key(event->keycode, shifted);
    if (!ascii) {
        return 0;
    }

    return (unsigned char)tolower((unsigned char)ascii);
}

static void dg_pump_events(void) {
    if (!s_window_ready || s_window_closed) {
        return;
    }

    for (;;) {
        ws_input_event_t events[16];
        ssize_t n = window_events(&s_window, events, 16);

        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                break;
            }

            if (errno == ENOENT) {
                dg_mark_window_closed();
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
    framebuffer_t *fb = window_buffer(&s_window);
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
        const uint32_t *src_row =
            src + (size_t)(src_y + y) * DOOMGENERIC_RESX + src_x;
        uint32_t *dst_row =
            fb->pixels + (size_t)(dst_y + y) * fb->width + dst_x;

        memcpy(dst_row, src_row, (size_t)copy_w * sizeof(uint32_t));
    }

    if (window_flush(&s_window) < 0) {
        if (errno == ENOENT) {
            dg_mark_window_closed();
        }
    }
}

void DG_Init(void) {
    memset(s_key_queue, 0, sizeof(s_key_queue));
    s_key_read = 0;
    s_key_write = 0;
    s_window_closed = false;
    s_close_key_sent = false;

    if (window_init(&s_window, DOOMGENERIC_RESX, DOOMGENERIC_RESY, "doom")) {
        fprintf(
            stderr,
            "doom: failed to create window (%d: %s)\n",
            errno,
            strerror(errno)
        );
        exit(1);
    }

    framebuffer_t *fb = window_buffer(&s_window);
    if (!fb || !fb->pixels) {
        fprintf(stderr, "doom: failed to acquire window framebuffer\n");
        window_deinit(&s_window);
        exit(1);
    }

    s_window_ready = true;

    if (clock_gettime(CLOCK_MONOTONIC, &s_ticks_start) < 0) {
        s_ticks_start.tv_sec = 0;
        s_ticks_start.tv_nsec = 0;
    }
}

void DG_DrawFrame(void) {
    dg_pump_events();

    if (s_window_closed) {
        _Exit(0);
    }

    dg_blit_frame();

    if (s_window_closed) {
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
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0;
    }

    uint64_t sec = 0;
    int64_t nsec = 0;

    if (now.tv_sec >= s_ticks_start.tv_sec) {
        sec = (uint64_t)(now.tv_sec - s_ticks_start.tv_sec);
    }

    nsec = now.tv_nsec - s_ticks_start.tv_nsec;
    if (nsec < 0) {
        if (sec > 0) {
            sec--;
        }
        nsec += 1000000000LL;
    }

    uint64_t ms = sec * 1000ULL + (uint64_t)(nsec / 1000000LL);
    return (uint32_t)ms;
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    dg_pump_events();

    if (s_window_closed && !s_close_key_sent) {
        dg_key_push(1, KEY_ESCAPE);
        s_close_key_sent = true;
    }

    return dg_key_pop(pressed, doomKey);
}

void DG_SetWindowTitle(const char *title) {
    if (!s_window_ready || s_window_closed) {
        return;
    }

    if (!title || !title[0]) {
        title = "doom";
    }

    if (window_set_title(&s_window, title) < 0 && errno == ENOENT) {
        dg_mark_window_closed();
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
