#include <draw.h>
#include <errno.h>
#include <input/kbd.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ui.h>

#define MBROT_RES_ADJUST_MIN (-1)
#define MBROT_RES_ADJUST_MAX 4
#define MBROT_MAX_ITER_LIMIT 288U
#define MBROT_RENDER_STEP_MIN 1U
#define MBROT_RENDER_STEP_MAX 16U
#define MBROT_HUD_MARGIN     6
#define MBROT_HUD_PAD_X      6
#define MBROT_HUD_PAD_Y      4
#define MBROT_HUD_LINE_GAP   2
#define MBROT_HUD_BG_NORMAL  DRAW_BLACK
#define MBROT_HUD_BG_RENDERING 0x00330000U
#define MBROT_SCALE_MIN      0.000000000001
#define MBROT_SCALE_MAX      8.0

typedef struct {
    double center_x;
    double center_y;
    double scale;
} mandelbrot_view_t;

typedef enum {
    MBROT_ACTION_NONE = 0,
    MBROT_ACTION_REDRAW = 1,
    MBROT_ACTION_RESOLUTION = 2,
    MBROT_ACTION_QUIT = -1,
} mbrot_action_t;

typedef enum {
    MBROT_FRACTAL_MANDELBROT = 0,
    MBROT_FRACTAL_JULIA = 1,
    MBROT_FRACTAL_BURNING_SHIP = 2,
    MBROT_FRACTAL_TRICORN = 3,
} mbrot_fractal_t;

typedef struct {
    mbrot_fractal_t fractal;
} mbrot_options_t;

typedef u32 (*mbrot_iter_fn_t)(double cx, double cy, u32 max_iter);

typedef struct {
    framebuffer_t *fb;
    size_t stride_pixels;
    mbrot_iter_fn_t iter_fn;
    u32 step;
    u32 max_iter;
    u32 color_lut[MBROT_MAX_ITER_LIMIT + 1];
    double x_min;
    double y_max;
    double dx;
    double dy;
    double step_dx;
} mbrot_render_ctx_t;

typedef struct {
    const char *title;
    double center_x;
    double center_y;
    double scale;
} mbrot_fractal_profile_t;

typedef struct {
    const char *arg;
    mbrot_fractal_t fractal;
} mbrot_arg_map_t;

static const mbrot_fractal_profile_t fractal_profiles[] = {
    [MBROT_FRACTAL_MANDELBROT] =
        {.title = "mbrot | mandelbrot", .center_x = -0.5, .center_y = 0.0, .scale = 1.6},
    [MBROT_FRACTAL_JULIA] =
        {.title = "mbrot | julia", .center_x = 0.0, .center_y = 0.0, .scale = 1.6},
    [MBROT_FRACTAL_BURNING_SHIP] =
        {.title = "mbrot | burning ship", .center_x = -0.45, .center_y = 0.5, .scale = 1.7},
    [MBROT_FRACTAL_TRICORN] =
        {.title = "mbrot | tricorn", .center_x = -0.2, .center_y = 0.0, .scale = 1.7},
};

static const mbrot_arg_map_t fractal_arg_map[] = {
    {.arg = "--mandelbrot", .fractal = MBROT_FRACTAL_MANDELBROT},
    {.arg = "--julia", .fractal = MBROT_FRACTAL_JULIA},
    {.arg = "--burning-ship", .fractal = MBROT_FRACTAL_BURNING_SHIP},
    {.arg = "--tricorn", .fractal = MBROT_FRACTAL_TRICORN},
};

static const mbrot_fractal_profile_t *fractal_profile(mbrot_fractal_t fractal) {
    u32 idx = (u32)fractal;
    if (idx >= (sizeof(fractal_profiles) / sizeof(fractal_profiles[0]))) {
        idx = MBROT_FRACTAL_MANDELBROT;
    }
    return &fractal_profiles[idx];
}

static void print_usage(const char *prog) {
    const char *name = (prog && prog[0]) ? prog : "mbrot";
    printf(
        "usage: %s [options]\n"
        "  --julia         julia set\n"
        "  --burning-ship  burning ship fractal\n"
        "  --tricorn       tricorn fractal\n"
        "  --mandelbrot    mandelbrot set\n"
        "  -h, --help      show this help\n",
        name
    );
}

static int parse_args(int argc, char **argv, mbrot_options_t *out) {
    if (!out) {
        return -1;
    }

    out->fractal = MBROT_FRACTAL_MANDELBROT;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!arg) {
            continue;
        }

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            print_usage(argv ? argv[0] : NULL);
            return 0;
        }

        bool matched = false;
        for (size_t j = 0; j < (sizeof(fractal_arg_map) / sizeof(fractal_arg_map[0])); j++) {
            if (strcmp(arg, fractal_arg_map[j].arg)) {
                continue;
            }

            out->fractal = fractal_arg_map[j].fractal;
            matched = true;
            break;
        }

        if (matched) {
            continue;
        }

        printf("mbrot: unknown option '%s'\n", arg);
        print_usage(argv ? argv[0] : NULL);
        return -1;
    }

    return 1;
}


static size_t fb_stride_pixels(const framebuffer_t *fb) {
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

static u32 clamp_u8_i32(int value) {
    return (u32)((value < 0) ? 0 : ((value > 255) ? 255 : value));
}

static void view_reset(mandelbrot_view_t *view, mbrot_fractal_t fractal) {
    if (!view) {
        return;
    }

    const mbrot_fractal_profile_t *profile = fractal_profile(fractal);
    view->center_x = profile->center_x;
    view->center_y = profile->center_y;
    view->scale = profile->scale;
}

static void view_clamp(mandelbrot_view_t *view) {
    if (!view) {
        return;
    }

    if (view->scale < MBROT_SCALE_MIN) {
        view->scale = MBROT_SCALE_MIN;
    }

    if (view->scale > MBROT_SCALE_MAX) {
        view->scale = MBROT_SCALE_MAX;
    }
}

static u32 compute_max_iter(double scale) {
    u32 iter = 72;
    double s = scale;

    while (s < 1.0 && iter < MBROT_MAX_ITER_LIMIT) {
        s *= 2.0;
        iter += 12;
    }

    if (iter > MBROT_MAX_ITER_LIMIT) {
        iter = MBROT_MAX_ITER_LIMIT;
    }

    return iter;
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

static u32 clamp_render_step(u32 step) {
    if (step < MBROT_RENDER_STEP_MIN) {
        return MBROT_RENDER_STEP_MIN;
    }

    if (step > MBROT_RENDER_STEP_MAX) {
        return MBROT_RENDER_STEP_MAX;
    }

    return step;
}

static u32 compute_render_step(
    u32 width,
    u32 height,
    int resolution_adjust
) {
    u32 step = ((u64)width * (u64)height >= 260000ULL) ? 2U : 1U;

    if (resolution_adjust < 0) {
        int inc = -resolution_adjust;
        for (int i = 0; i < inc; i++) {
            if (step > MBROT_RENDER_STEP_MIN) {
                step >>= 1U;
            }
        }
    } else if (resolution_adjust > 0) {
        for (int i = 0; i < resolution_adjust; i++) {
            if (step >= MBROT_RENDER_STEP_MAX) {
                break;
            }
            step <<= 1U;
        }
    }

    return clamp_render_step(step);
}

static u32 progressive_next_step(u32 current_step, u32 target_step) {
    current_step = clamp_render_step(current_step);
    target_step = clamp_render_step(target_step);

    if (current_step == target_step) {
        return target_step;
    }

    if (current_step > target_step) {
        u32 next = current_step >> 1U;
        if (next < target_step || next == current_step) {
            next = target_step;
        }
        return clamp_render_step(next);
    }

    u32 next = current_step << 1U;
    if (next < current_step || next > target_step) {
        next = target_step;
    }

    return clamp_render_step(next);
}

static void draw_status_overlay(
    framebuffer_t *fb,
    const mandelbrot_view_t *view,
    mbrot_fractal_t fractal,
    u32 current_step,
    u32 target_step,
    bool rendering
) {
    if (!fb || !view || !fb->width || !fb->height) {
        return;
    }

    double zoom = 1.0;
    double base_scale = fractal_profile(fractal)->scale;
    if (view->scale > 0.0) {
        zoom = base_scale / view->scale;
    }

    char zoom_line[48] = {0};
    char res_line[48] = {0};
    u32 zoom_x100 = 100U;
    if (zoom > 0.0) {
        double scaled = (zoom * 100.0) + 0.5;
        if (scaled < 0.0) {
            scaled = 0.0;
        }
        if (scaled > 999999.0) {
            scaled = 999999.0;
        }
        zoom_x100 = (u32)scaled;
    }

    u32 zoom_whole = zoom_x100 / 100U;
    u32 zoom_frac = zoom_x100 % 100U;

    snprintf(zoom_line, sizeof(zoom_line), "zoom: %u.%02ux", zoom_whole, zoom_frac);
    snprintf(
        res_line,
        sizeof(res_line),
        "res: step %u / %u",
        current_step,
        target_step
    );

    int zoom_w = text_width_px(zoom_line);
    int res_w = text_width_px(res_line);
    int text_w = zoom_w > res_w ? zoom_w : res_w;
    int line_count = 2;
    int text_height = text_height_px();
    int text_h =
        (line_count * text_height) + ((line_count - 1) * MBROT_HUD_LINE_GAP);

    int box_w = text_w + (2 * MBROT_HUD_PAD_X);
    int box_h = text_h + (2 * MBROT_HUD_PAD_Y);
    int box_x = (int)fb->width - MBROT_HUD_MARGIN - box_w;
    int box_y = MBROT_HUD_MARGIN;

    if (box_x < MBROT_HUD_MARGIN) {
        box_x = MBROT_HUD_MARGIN;
    }

    if (box_y + box_h > (int)fb->height) {
        box_y = (int)fb->height - box_h;
        if (box_y < 0) {
            box_y = 0;
        }
    }

    u32 box_bg = rendering ? MBROT_HUD_BG_RENDERING : MBROT_HUD_BG_NORMAL;
    draw_rect(fb, box_x, box_y, (u32)box_w, (u32)box_h, box_bg);

    int text_x = box_x + MBROT_HUD_PAD_X;
    int zoom_y = box_y + MBROT_HUD_PAD_Y;
    int res_y = zoom_y + text_height + MBROT_HUD_LINE_GAP;

    draw_text(fb, text_x, zoom_y, zoom_line, DRAW_WHITE);
    draw_text(fb, text_x, res_y, res_line, DRAW_WHITE);
}

static u32 palette_color(u32 iter, u32 max_iter) {
    if (iter >= max_iter) {
        return 0x00000000U;
    }

    double t = (double)iter / (double)max_iter;

    int r = 0;
    int g = 0;
    int b = 0;

    u32 wheel = (u32)(((u64)iter * 1536ULL) / (u64)max_iter);
    u32 seg = wheel >> 8;
    u32 x = wheel & 0xffU;

    switch (seg) {
    case 0:
        r = 255;
        g = (int)x;
        b = 0;
        break;
    case 1:
        r = 255 - (int)x;
        g = 255;
        b = 0;
        break;
    case 2:
        r = 0;
        g = 255;
        b = (int)x;
        break;
    case 3:
        r = 0;
        g = 255 - (int)x;
        b = 255;
        break;
    case 4:
        r = (int)x;
        g = 0;
        b = 255;
        break;
    default:
        r = 255;
        g = 0;
        b = 255 - (int)x;
        break;
    }

    int shade = (int)(88.0 + (167.0 * t));
    r = (r * shade) / 255;
    g = (g * shade) / 255;
    b = (b * shade) / 255;

    return (clamp_u8_i32(r) << 16) | (clamp_u8_i32(g) << 8) |
           clamp_u8_i32(b);
}

static bool point_inside_main_body(double cx, double cy) {
    double x = cx - 0.25;
    double y2 = cy * cy;
    double q = x * x + y2;

    if (q * (q + x) <= 0.25 * y2) {
        return true;
    }

    double bx = cx + 1.0;
    if (bx * bx + y2 <= 0.0625) {
        return true;
    }

    return false;
}

static u32 iterate_mandelbrot(double cx, double cy, u32 max_iter) {
    if (point_inside_main_body(cx, cy)) {
        return max_iter;
    }

    double zx = 0.0;
    double zy = 0.0;
    u32 iter = 0;

    while (iter < max_iter) {
        double zx2 = zx * zx;
        double zy2 = zy * zy;

        if (zx2 + zy2 > 4.0) {
            break;
        }

        double zxy = zx * zy;
        zy = zxy + zxy + cy;
        zx = zx2 - zy2 + cx;
        iter++;
    }

    return iter;
}

static u32 iterate_julia(double cx, double cy, u32 max_iter) {
    double zx = cx;
    double zy = cy;
    const double c_re = -0.8;
    const double c_im = 0.156;
    u32 iter = 0;

    while (iter < max_iter) {
        double zx2 = zx * zx;
        double zy2 = zy * zy;

        if (zx2 + zy2 > 4.0) {
            break;
        }

        double zxy = zx * zy;
        zy = zxy + zxy + c_im;
        zx = zx2 - zy2 + c_re;
        iter++;
    }

    return iter;
}

static u32 iterate_burning_ship(double cx, double cy, u32 max_iter) {
    double zx = 0.0;
    double zy = 0.0;
    u32 iter = 0;

    while (iter < max_iter) {
        double zx2 = zx * zx;
        double zy2 = zy * zy;

        if (zx2 + zy2 > 4.0) {
            break;
        }

        double ax = fabs(zx);
        double ay = fabs(zy);
        zy = cy - (ax + ax) * ay;
        zx = ax * ax - ay * ay + cx;
        iter++;
    }

    return iter;
}

static u32 iterate_tricorn(double cx, double cy, u32 max_iter) {
    double zx = 0.0;
    double zy = 0.0;
    u32 iter = 0;

    while (iter < max_iter) {
        double zx2 = zx * zx;
        double zy2 = zy * zy;

        if (zx2 + zy2 > 4.0) {
            break;
        }

        double zxy = zx * zy;
        zy = cy - (zxy + zxy);
        zx = zx2 - zy2 + cx;
        iter++;
    }

    return iter;
}

static mbrot_iter_fn_t fractal_iter_fn(mbrot_fractal_t fractal) {
    switch (fractal) {
    case MBROT_FRACTAL_JULIA:
        return iterate_julia;
    case MBROT_FRACTAL_BURNING_SHIP:
        return iterate_burning_ship;
    case MBROT_FRACTAL_TRICORN:
        return iterate_tricorn;
    case MBROT_FRACTAL_MANDELBROT:
    default:
        return iterate_mandelbrot;
    }
}

static bool init_render_ctx(
    framebuffer_t *fb,
    const mandelbrot_view_t *view,
    mbrot_fractal_t fractal,
    u32 step,
    mbrot_render_ctx_t *ctx
) {
    if (!fb || !fb->pixels || !view || !ctx || !fb->width || !fb->height) {
        return false;
    }

    size_t stride_pixels = fb_stride_pixels(fb);
    if (!stride_pixels) {
        return false;
    }

    ctx->fb = fb;
    ctx->stride_pixels = stride_pixels;
    ctx->iter_fn = fractal_iter_fn(fractal);
    ctx->step = clamp_render_step(step);
    ctx->max_iter = compute_max_iter(view->scale);
    for (u32 i = 0; i <= ctx->max_iter; i++) {
        ctx->color_lut[i] = palette_color(i, ctx->max_iter);
    }

    double aspect = (double)fb->height / (double)fb->width;
    ctx->x_min = view->center_x - view->scale;
    ctx->y_max = view->center_y + view->scale * aspect;
    ctx->dx = (2.0 * view->scale) / (double)fb->width;
    ctx->dy = (2.0 * view->scale * aspect) / (double)fb->height;
    ctx->step_dx = ctx->dx * (double)ctx->step;

    return true;
}

static void render_row_pixels(const mbrot_render_ctx_t *ctx, u32 y) {
    framebuffer_t *fb = ctx->fb;
    size_t row = (size_t)y * ctx->stride_pixels;
    double cy = ctx->y_max - ((double)y * ctx->dy);
    double cx = ctx->x_min;

    for (u32 x = 0; x < fb->width; x++) {
        u32 iter = ctx->iter_fn(cx, cy, ctx->max_iter);
        fb->pixels[row + x] = ctx->color_lut[iter];
        cx += ctx->dx;
    }
}

static void render_row_blocks(const mbrot_render_ctx_t *ctx, u32 y) {
    framebuffer_t *fb = ctx->fb;
    double cy = ctx->y_max - ((double)y * ctx->dy);
    double cx = ctx->x_min;

    for (u32 x = 0; x < fb->width; x += ctx->step) {
        u32 iter = ctx->iter_fn(cx, cy, ctx->max_iter);
        u32 color = ctx->color_lut[iter];

        u32 x_end = x + ctx->step;
        if (x_end > fb->width) {
            x_end = fb->width;
        }

        u32 y_end = y + ctx->step;
        if (y_end > fb->height) {
            y_end = fb->height;
        }

        draw_rect(fb, (i32)x, (i32)y, x_end - x, y_end - y, color);
        cx += ctx->step_dx;
    }
}

static bool flush_frame(window_t *window) {
    return window && window_flush(window) == 0;
}

static bool render_fractal_pass(
    window_t *window,
    const mandelbrot_view_t *view,
    mbrot_fractal_t fractal,
    u32 step,
    u32 target_step
) {
    if (!window || !view) {
        return false;
    }

    framebuffer_t *fb = window_buffer(window);
    if (!fb) {
        return false;
    }

    mbrot_render_ctx_t ctx = {0};
    if (!init_render_ctx(fb, view, fractal, step, &ctx)) {
        return false;
    }
    target_step = clamp_render_step(target_step);

    void (*render_row)(const mbrot_render_ctx_t *, u32) =
        (ctx.step == 1U) ? render_row_pixels : render_row_blocks;

    draw_status_overlay(ctx.fb, view, fractal, ctx.step, target_step, true);
    if (!flush_frame(window) && errno != EAGAIN && errno != EINTR) {
        return false;
    }

    for (u32 y = 0; y < ctx.fb->height; y += ctx.step) {
        render_row(&ctx, y);
    }

    draw_status_overlay(ctx.fb, view, fractal, ctx.step, target_step, false);
    return true;
}

static bool render_current_resolution(
    window_t *window,
    const mandelbrot_view_t *view,
    mbrot_fractal_t fractal,
    int resolution_adjust
) {
    if (!window) {
        return false;
    }

    framebuffer_t *fb = window_buffer(window);
    if (!fb) {
        return false;
    }

    u32 step = compute_render_step(fb->width, fb->height, resolution_adjust);
    return render_fractal_pass(window, view, fractal, step, step);
}

static int
handle_event(
    mandelbrot_view_t *view,
    mbrot_fractal_t fractal,
    int *resolution_adjust,
    const ws_input_event_t *event
) {
    if (!view || !resolution_adjust || !event) {
        return MBROT_ACTION_NONE;
    }

    if (event->type == INPUT_EVENT_WINDOW_RESIZE) {
        return MBROT_ACTION_REDRAW;
    }

    if (event->type != INPUT_EVENT_KEY || !event->action) {
        return MBROT_ACTION_NONE;
    }

    double pan_step = view->scale * 0.18;

    switch (event->keycode) {
    case KBD_ESCAPE:
    case KBD_Q:
        return MBROT_ACTION_QUIT;
    case KBD_EQUALS:
    case KBD_KP_PLUS:
        view->scale *= 0.84;
        view_clamp(view);
        break;
    case KBD_MINUS:
    case KBD_KP_MINUS:
        view->scale *= 1.20;
        view_clamp(view);
        break;
    case KBD_LEFT:
        view->center_x -= pan_step;
        break;
    case KBD_RIGHT:
        view->center_x += pan_step;
        break;
    case KBD_UP:
        view->center_y += pan_step;
        break;
    case KBD_DOWN:
        view->center_y -= pan_step;
        break;
    case KBD_0:
    case KBD_R:
        view_reset(view, fractal);
        break;
    case KBD_LEFT_BRACKET:
    case KBD_COMMA:
        if (*resolution_adjust > MBROT_RES_ADJUST_MIN) {
            (*resolution_adjust)--;
            return MBROT_ACTION_RESOLUTION;
        }
        return MBROT_ACTION_NONE;
    case KBD_RIGHT_BRACKET:
    case KBD_DOT:
        if (*resolution_adjust < MBROT_RES_ADJUST_MAX) {
            (*resolution_adjust)++;
            return MBROT_ACTION_RESOLUTION;
        }
        return MBROT_ACTION_NONE;
    default:
        return MBROT_ACTION_NONE;
    }

    return MBROT_ACTION_REDRAW;
}

static bool flush_or_set_pending(window_t *window, bool *present_pending) {
    if (!present_pending) {
        return false;
    }

    if (flush_frame(window)) {
        *present_pending = false;
        return true;
    }

    if (errno == EAGAIN || errno == EINTR) {
        *present_pending = true;
        return true;
    }

    return false;
}

static void advance_progressive_state(
    bool *progressive_pending,
    u32 *progressive_step,
    u32 progressive_target_step
) {
    if (!progressive_pending || !progressive_step) {
        return;
    }

    if (*progressive_step == progressive_target_step) {
        *progressive_pending = false;
        return;
    }

    *progressive_step = progressive_next_step(
        *progressive_step,
        progressive_target_step
    );
}

int main(int argc, char **argv) {
    mbrot_options_t opts = {0};
    int parse_status = parse_args(argc, argv, &opts);
    if (parse_status <= 0) {
        return parse_status < 0 ? 1 : 0;
    }

    window_t window = {0};
    if (window_init(&window, 760, 500, fractal_profile(opts.fractal)->title)) {
        return 1;
    }

    printf("+/- zoom, arrows pan, ] decrease res [ increase res, r reset, q/esc quit\n");

    mandelbrot_view_t view = {0};
    int resolution_adjust = 0;
    view_reset(&view, opts.fractal);

    if (!render_current_resolution(
            &window,
            &view,
            opts.fractal,
            resolution_adjust
        )) {
        window_deinit(&window);
        return 1;
    }
    bool present_pending = false;
    if (!flush_or_set_pending(&window, &present_pending)) {
        window_deinit(&window);
        return 1;
    }

    bool progressive_pending = false;
    u32 progressive_step = 1U;
    u32 progressive_target_step = 1U;

    while (true) {
        int timeout_ms = (progressive_pending || present_pending) ? 0 : -1;

        ws_input_event_t event = {0};
        int ret = window_wait_event(&window, &event, timeout_ms);

        if (ret < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            break;
        }

        if (ret == 0) {
            if (progressive_pending) {
                if (!render_fractal_pass(
                    &window,
                    &view,
                    opts.fractal,
                    progressive_step,
                    progressive_target_step
                )) {
                    break;
                }

                if (!flush_or_set_pending(&window, &present_pending)) {
                    break;
                }
                if (present_pending) {
                    continue;
                }

                advance_progressive_state(
                    &progressive_pending,
                    &progressive_step,
                    progressive_target_step
                );
                continue;
            }

            if (present_pending) {
                if (!flush_or_set_pending(&window, &present_pending)) {
                    break;
                }
            }
            continue;
        }

        int prev_resolution_adjust = resolution_adjust;
        int action = handle_event(&view, opts.fractal, &resolution_adjust, &event);

        if (action == MBROT_ACTION_QUIT) {
            break;
        }

        if (action == MBROT_ACTION_NONE) {
            continue;
        }

        if (action == MBROT_ACTION_REDRAW) {
            progressive_pending = false;
            if (!render_current_resolution(
                    &window,
                    &view,
                    opts.fractal,
                    resolution_adjust
                )) {
                break;
            }
        } else if (action == MBROT_ACTION_RESOLUTION) {
            framebuffer_t *fb = window_buffer(&window);
            u32 width = fb ? fb->width : 0U;
            u32 height = fb ? fb->height : 0U;
            u32 old_step = compute_render_step(
                width,
                height,
                prev_resolution_adjust
            );
            u32 new_step =
                compute_render_step(width, height, resolution_adjust);
            u32 start_step = old_step > new_step ? old_step : new_step;

            progressive_target_step = new_step;
            progressive_step = start_step;
            progressive_pending = true;

            if (!render_fractal_pass(
                &window,
                &view,
                opts.fractal,
                progressive_step,
                progressive_target_step
            )) {
                break;
            }

            advance_progressive_state(
                &progressive_pending,
                &progressive_step,
                progressive_target_step
            );
        }

        if (!flush_or_set_pending(&window, &present_pending)) {
            break;
        }
    }

    window_deinit(&window);
    return 0;
}
