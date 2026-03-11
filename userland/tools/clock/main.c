#include <draw.h>
#include <errno.h>
#include <input/kbd.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ui.h>

typedef struct {
    int x;
    int y;
} point_t;

static const double k_pi = 3.14159265358979323846;
static const int k_clock_face_label_gap = 6;
static const int k_clock_edge_pad = 4;
static const int k_clock_label_raise = 8;
static const int k_clock_smooth_frame_ms = 50;

typedef struct {
    bool show_date;
    bool smooth;
    bool numbered;
} clock_options_t;

static void print_usage(const char *prog) {
    const char *name = (prog && prog[0]) ? prog : "clock";
    printf(
        "usage: %s [options]\n"
        "  -d, --no-date  hide date/year labels\n"
        "  -n, --numbered place numbers at hour ticks\n"
        "  -s, --smooth   animate second/minute/hour hands smoothly\n"
        "  -h, --help     show this help\n",
        name
    );
}

static int parse_args(int argc, char **argv, clock_options_t *out) {
    if (!out) {
        return -1;
    }

    out->show_date = true;
    out->smooth = false;
    out->numbered = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!arg) {
            continue;
        }

        if (!strcmp(arg, "-d") || !strcmp(arg, "--no-date")) {
            out->show_date = false;
            continue;
        }

        if (!strcmp(arg, "-n") || !strcmp(arg, "--numbered")) {
            out->numbered = true;
            continue;
        }

        if (!strcmp(arg, "-s") || !strcmp(arg, "--smooth")) {
            out->smooth = true;
            continue;
        }

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            print_usage(argv ? argv[0] : NULL);
            return 0;
        }

        printf("clock: unknown option '%s'\n", arg);
        print_usage(argv ? argv[0] : NULL);
        return -1;
    }

    return 1;
}

static int text_width_px(const char *text) {
    if (!text) {
        return 0;
    }

    int advance = (int)draw_font_width();
    if (advance < 1) {
        advance = 1;
    }

    size_t len = strlen(text);
    return (int)len * advance;
}

static int text_height_px(void) {
    int height = (int)draw_font_height();
    if (height < 1) {
        height = 1;
    }

    return height;
}

static void draw_face_labels(
    framebuffer_t *fb,
    int width,
    int cx,
    int cy,
    int radius,
    const struct tm *tm_now
) {
    if (!fb || !tm_now || width <= 0 || radius <= 0) {
        return;
    }

    char year_text[16] = {0};
    char date_text[16] = {0};

    if (!strftime(year_text, sizeof(year_text), "%Y", tm_now)) {
        snprintf(year_text, sizeof(year_text), "?");
    }
    if (!strftime(date_text, sizeof(date_text), "%b %d", tm_now)) {
        snprintf(date_text, sizeof(date_text), "??? ??");
    }

    int date_w = text_width_px(date_text);
    int year_w = text_width_px(year_text);

    int face_left = cx - radius;
    int face_right = cx + radius;
    int text_height = text_height_px();
    int label_y =
        cy - radius - k_clock_face_label_gap - text_height - k_clock_label_raise;
    if (label_y < 2) {
        label_y = 2;
    }

    int date_x = face_left;
    if (date_x < k_clock_edge_pad) {
        date_x = k_clock_edge_pad;
    }

    int year_x = face_right - year_w;
    if (year_x + year_w > width - k_clock_edge_pad) {
        year_x = width - k_clock_edge_pad - year_w;
    }
    if (year_x < k_clock_edge_pad) {
        year_x = k_clock_edge_pad;
    }

    if (date_x + date_w + k_clock_edge_pad > year_x) {
        date_x = k_clock_edge_pad;
        year_x = width - k_clock_edge_pad - year_w;
    }

    draw_text(fb, date_x, label_y, date_text, DRAW_WHITE);
    draw_text(fb, year_x, label_y, year_text, DRAW_WHITE);
}

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

static int tick_milli_from_scale(double value, double scale) {
    if (scale <= 0.0) {
        return 0;
    }

    double wrapped = fmod(value, scale);
    if (wrapped < 0.0) {
        wrapped += scale;
    }

    int tick_milli = (int)((wrapped * 60000.0) / scale);
    if (tick_milli < 0) {
        return 0;
    }
    if (tick_milli >= 60000) {
        return 59999;
    }

    return tick_milli;
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

static void draw_hour_numbers(framebuffer_t *fb, int cx, int cy, int radius) {
    if (!fb || radius <= 0) {
        return;
    }

    int inset = clamp_i32((radius / 7) + 5, 18, 28);
    int label_radius = radius - inset;

    for (int hour = 1; hour <= 12; hour++) {
        int tick_index = (hour % 12) * 5;
        point_t p = polar_point_tick(cx, cy, label_radius, tick_index * 1000);

        char text[4];
        snprintf(text, sizeof(text), "%d", hour);

        int text_w = text_width_px(text);
        int text_x = p.x - (text_w / 2);
        int text_y = p.y - (text_height_px() / 2);
        draw_text(fb, text_x, text_y, text, DRAW_WHITE);
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

static bool
draw_clock(
    window_t *window,
    const struct tm *tm_now,
    const clock_options_t *opts,
    double second_fraction
) {
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
    bool show_date = !opts || opts->show_date;
    int clock_top = k_clock_edge_pad;

    if (show_date) {
        clock_top += text_height_px() + k_clock_face_label_gap + k_clock_edge_pad;
    }

    int available_h = height - clock_top;
    if (available_h < 1) {
        available_h = 1;
    }

    int cy = clock_top + (available_h / 2);

    int min_dim = imin(width, height);
    int margin = min_dim / 12;
    margin = clamp_i32(margin, 8, 20);

    int radius_w = (width / 2) - margin;
    int radius_h = (available_h / 2) - margin;
    int radius = imin(radius_w, radius_h);
    if (radius < 8) {
        radius = 8;
    }

    if (show_date) {
        draw_face_labels(fb, width, cx, cy, radius, tm_now);
    }
    draw_ticks(fb, cx, cy, radius);
    if (opts && opts->numbered) {
        draw_hour_numbers(fb, cx, cy, radius);
    }

    if (second_fraction < 0.0) {
        second_fraction = 0.0;
    } else if (second_fraction > 0.999999) {
        second_fraction = 0.999999;
    }

    double second_pos = (double)tm_now->tm_sec + second_fraction;
    double minute_pos = (double)tm_now->tm_min + (second_pos / 60.0);
    double hour_pos = (double)(tm_now->tm_hour % 12) + (minute_pos / 60.0);

    int second_tick_milli = tick_milli_from_scale(second_pos, 60.0);
    int minute_tick_milli = tick_milli_from_scale(minute_pos, 60.0);
    int hour_tick_milli = tick_milli_from_scale(hour_pos, 12.0);

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

typedef struct {
    bool valid;
    time_t display_sec;
    struct timespec mono_mark;
} smooth_clock_state_t;

static bool smooth_clock_sample(
    time_t wall_sec,
    smooth_clock_state_t *state,
    time_t *display_sec_out,
    double *second_fraction_out
) {
    if (!state || !display_sec_out || !second_fraction_out || wall_sec == (time_t)-1) {
        return false;
    }

    struct timespec mono_now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &mono_now) < 0) {
        return false;
    }

    if (!state->valid) {
        state->valid = true;
        state->display_sec = wall_sec;
        state->mono_mark = mono_now;
        *display_sec_out = wall_sec;
        *second_fraction_out = 0.0;
        return true;
    }

    time_t ds = mono_now.tv_sec - state->mono_mark.tv_sec;
    long dns = mono_now.tv_nsec - state->mono_mark.tv_nsec;
    double elapsed = (double)ds + ((double)dns / 1000000000.0);

    if (elapsed < 0.0 || elapsed > 4.0) {
        state->display_sec = wall_sec;
        state->mono_mark = mono_now;
        *display_sec_out = wall_sec;
        *second_fraction_out = 0.0;
        return true;
    }

    while (elapsed >= 1.0) {
        state->display_sec++;
        state->mono_mark.tv_sec++;
        elapsed -= 1.0;
    }

    if (wall_sec > state->display_sec || wall_sec + 1 < state->display_sec) {
        state->display_sec = wall_sec;
        state->mono_mark = mono_now;
        elapsed = 0.0;
    }

    if (elapsed < 0.0) {
        elapsed = 0.0;
    }
    if (elapsed > 0.999999) {
        elapsed = 0.999999;
    }

    *display_sec_out = state->display_sec;
    *second_fraction_out = elapsed;
    return true;
}

int main(int argc, char **argv) {
    clock_options_t opts = {0};
    int parse_status = parse_args(argc, argv, &opts);
    if (parse_status <= 0) {
        return parse_status < 0 ? 1 : 0;
    }

    window_t window = {0};
    if (window_init(&window, 320, 360, "clock")) {
        return 1;
    }

    time_t last_sec = (time_t)-1;
    bool redraw = true;
    bool running = true;
    smooth_clock_state_t smooth_state = {0};

    while (running) {
        time_t now = time(NULL);
        time_t display_sec = now;
        double second_fraction = 0.0;

        if (opts.smooth && now != (time_t)-1) {
            if (!smooth_clock_sample(now, &smooth_state, &display_sec, &second_fraction)) {
                break;
            }
            redraw = true;
        }

        if (display_sec != (time_t)-1 && display_sec != last_sec) {
            last_sec = display_sec;
            redraw = true;
        }

        if (redraw) {
            struct tm tm_now = {0};

            if (!clock_tm_from_time(display_sec, &tm_now)) {
                break;
            }

            if (draw_clock(&window, &tm_now, &opts, second_fraction)) {
                redraw = false;
            } else if (errno != EAGAIN && errno != EINTR) {
                break;
            }
        }

        ws_input_event_t event = {0};

        int timeout_ms = opts.smooth ? k_clock_smooth_frame_ms : (redraw ? 0 : 100);

        int rc = window_wait_event(&window, &event, timeout_ms);

        if (rc < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
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
