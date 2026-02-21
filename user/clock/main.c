#include <draw.h>
#include <errno.h>
#include <input/kbd.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>
#include <ui.h>

typedef struct {
    int x;
    int y;
} point_t;

static const double k_pi = 3.14159265358979323846;

static int clamp_i32(int value, int low, int high) {
    if (value < low) {
        return low;
    }

    if (value > high) {
        return high;
    }

    return value;
}

static int imin(int a, int b) {
    if (a < b) {
        return a;
    }

    return b;
}

static double angle_from_tick_milli(int tick_milli) {
    double tick = (double)tick_milli / 1000.0;
    return ((2.0 * k_pi * tick) / 60.0) - (k_pi / 2.0);
}

static point_t polar_point_tick(int cx, int cy, int radius, int tick_milli) {
    point_t out = {0};

    double angle = angle_from_tick_milli(tick_milli);

    double fx = (double)cx + cos(angle) * (double)radius;
    double fy = (double)cy + sin(angle) * (double)radius;

    if (fx >= 0.0) {
        out.x = (int)(fx + 0.5);
    } else {
        out.x = (int)(fx - 0.5);
    }

    if (fy >= 0.0) {
        out.y = (int)(fy + 0.5);
    } else {
        out.y = (int)(fy - 0.5);
    }

    return out;
}

static void draw_ticks(framebuffer_t *fb, int cx, int cy, int radius) {
    for (int i = 0; i < 60; i++) {
        int inner = radius;

        if ((i % 5) == 0) {
            inner -= clamp_i32(radius / 12, 8, 16);
        } else {
            inner -= clamp_i32(radius / 20, 4, 10);
        }

        int tick_milli = i * 1000;
        point_t p0 = polar_point_tick(cx, cy, inner, tick_milli);
        point_t p1 = polar_point_tick(cx, cy, radius, tick_milli);
        draw_line(fb, p0.x, p0.y, p1.x, p1.y, DRAW_WHITE);
    }
}

static void draw_hand(
    framebuffer_t *fb,
    int cx,
    int cy,
    int radius,
    int tick_milli,
    pixel_t color
) {
    point_t p = polar_point_tick(cx, cy, radius, tick_milli);
    draw_line(fb, cx, cy, p.x, p.y, color);
}

static bool clock_tm_from_time(time_t now, struct tm *tm_out) {
    if (!tm_out || now == (time_t)-1) {
        return false;
    }

    return localtime_r(&now, tm_out) != NULL;
}

static bool draw_clock(window_t *window, const struct tm *tm_now) {
    if (!window || !tm_now) {
        return false;
    }

    framebuffer_t *fb = window_buffer(window);
    if (!fb || !fb->pixels) {
        return false;
    }

    draw_rect(fb, 0, 0, fb->width, fb->height, DRAW_BLACK);

    int width = (int)fb->width;
    int height = (int)fb->height;
    int cx = width / 2;
    int cy = height / 2;

    int min_dim = imin(width, height);
    int margin = min_dim / 12;
    margin = clamp_i32(margin, 8, 20);

    int radius = (min_dim / 2) - margin;
    if (radius < 8) {
        radius = 8;
    }

    draw_ticks(fb, cx, cy, radius);

    int second_tick_milli = tm_now->tm_sec * 1000;
    int minute_tick_milli =
        (tm_now->tm_min * 1000) + ((tm_now->tm_sec * 1000) / 60);
    int hour = tm_now->tm_hour % 12;
    int hour_tick_milli =
        (hour * 5000) + ((tm_now->tm_min * 5000) / 60) + ((tm_now->tm_sec * 5000) / 3600);

    int hour_len = (radius * 50) / 100;
    int minute_len = (radius * 70) / 100;
    int second_len = (radius * 78) / 100;
    int second_tail = (radius * 10) / 100;

    draw_hand(fb, cx, cy, hour_len, hour_tick_milli, DRAW_WHITE);
    draw_hand(
        fb,
        cx,
        cy,
        minute_len,
        minute_tick_milli,
        DRAW_GRAY_LIGHT
    );
    draw_hand(fb, cx, cy, second_len, second_tick_milli, DRAW_RED);
    draw_hand(
        fb,
        cx,
        cy,
        second_tail,
        second_tick_milli + 30000,
        DRAW_RED
    );

    draw_rect(fb, cx - 1, cy - 1, 3, 3, DRAW_RED);

    if (window_flush(window) < 0) {
        return false;
    }

    return true;
}

static bool should_quit(const ws_input_event_t *event) {
    if (!event) {
        return false;
    }
    if (event->type != INPUT_EVENT_KEY || !event->action) {
        return false;
    }
    return event->keycode == KBD_ESCAPE;
}

int main(void) {
    window_t window = {0};
    if (window_init(&window, 320, 360, "clock")) {
        return 1;
    }

    time_t last_sec = (time_t)-1;
    bool redraw = true;
    bool running = true;

    while (running) {
        time_t now = time(NULL);

        if (now != (time_t)-1 && now != last_sec) {
            last_sec = now;
            redraw = true;
        }

        if (redraw) {
            struct tm tm_now = {0};

            if (!clock_tm_from_time(now, &tm_now)) {
                break;
            }

            if (draw_clock(&window, &tm_now)) {
                redraw = false;
            }
        }

        ws_input_event_t event = {0};

        int timeout_ms = 100;
        if (redraw) {
            timeout_ms = 0;
        }

        int rc = window_wait_event(&window, &event, timeout_ms);

        if (rc < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            if (errno == ENOENT) {
                break;
            }
            break;
        }

        while (rc > 0) {
            if (event.type == INPUT_EVENT_WINDOW_RESIZE) {
                redraw = true;
            } else if (should_quit(&event)) {
                running = false;
                break;
            }

            rc = window_wait_event(&window, &event, 0);

            if (rc < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    break;
                }

                if (errno == ENOENT) {
                    running = false;
                    break;
                }

                running = false;
                break;
            }
        }
    }

    window_deinit(&window);
    return 0;
}
