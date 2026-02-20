#include <errno.h>
#include <input/kbd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ui.h>
#include <unistd.h>

typedef struct {
    double center_x;
    double center_y;
    double scale;
} mandelbrot_view_t;

typedef enum {
    MBROT_ACTION_NONE = 0,
    MBROT_ACTION_RENDER = 1,
    MBROT_ACTION_RESIZE = 2,
    MBROT_ACTION_QUIT = -1,
} mbrot_action_t;


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

static void view_reset(mandelbrot_view_t *view) {
    if (!view) {
        return;
    }

    view->center_x = -0.5;
    view->center_y = 0.0;
    view->scale = 1.6;
}

static void view_clamp(mandelbrot_view_t *view) {
    if (!view) {
        return;
    }

    if (view->scale < 0.000000000001) {
        view->scale = 0.000000000001;
    }

    if (view->scale > 8.0) {
        view->scale = 8.0;
    }
}

static u32 compute_max_iter(double scale) {
    u32 iter = 72;
    double s = scale;

    while (s < 1.0 && iter < 288) {
        s *= 2.0;
        iter += 12;
    }

    return iter;
}

static u32 palette_color(u32 iter, u32 max_iter) {
    if (iter >= max_iter) {
        return 0x00000000U;
    }

    u32 t = (u32)(((u64)iter * 1536ULL) / (u64)max_iter);
    u32 seg = t >> 8;
    u32 x = t & 0xffU;

    u32 r = 0;
    u32 g = 0;
    u32 b = 0;

    switch (seg) {
    case 0:
        r = 255;
        g = x;
        b = 0;
        break;
    case 1:
        r = 255 - x;
        g = 255;
        b = 0;
        break;
    case 2:
        r = 0;
        g = 255;
        b = x;
        break;
    case 3:
        r = 0;
        g = 255 - x;
        b = 255;
        break;
    case 4:
        r = x;
        g = 0;
        b = 255;
        break;
    default:
        r = 255;
        g = 0;
        b = 255 - x;
        break;
    }

    u32 shade = 96U + (u32)(((u64)iter * 159ULL) / (u64)max_iter);
    r = (r * shade) / 255U;
    g = (g * shade) / 255U;
    b = (b * shade) / 255U;

    return (r << 16) | (g << 8) | b;
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

static void
fill_block(framebuffer_t *fb, u32 x, u32 y, u32 step, pixel_t color) {
    if (!fb || !fb->pixels || !fb->width || !fb->height || !step) {
        return;
    }

    size_t stride_pixels = fb_stride_pixels(fb);
    if (!stride_pixels) {
        return;
    }

    u32 y_end = y + step;
    u32 x_end = x + step;

    if (y_end > fb->height) {
        y_end = fb->height;
    }

    if (x_end > fb->width) {
        x_end = fb->width;
    }

    for (u32 yy = y; yy < y_end; yy++) {
        size_t row = (size_t)yy * stride_pixels;
        for (u32 xx = x; xx < x_end; xx++) {
            fb->pixels[row + xx] = color;
        }
    }
}

static bool flush_frame(window_t *window) {
    if (!window) {
        return false;
    }

    if (!window_flush(window)) {
        return true;
    }

    if (errno == EAGAIN || errno == EINTR) {
        return false;
    }

    return false;
}

static void render_mandelbrot(
    window_t *window,
    const mandelbrot_view_t *view,
    bool fast_preview
) {
    if (!window || !view) {
        return;
    }

    framebuffer_t *fb = window_buffer(window);
    if (!fb || !fb->pixels) {
        return;
    }

    u32 width = fb->width;
    u32 height = fb->height;
    if (!width || !height) {
        return;
    }

    u32 max_iter = compute_max_iter(view->scale);
    if (fast_preview) {
        max_iter /= 2U;
        if (max_iter < 48U) {
            max_iter = 48U;
        }
    }

    u32 step = ((u64)width * (u64)height >= 260000ULL) ? 2U : 1U;
    if (fast_preview) {
        step *= 2U;
        if (step > 4U) {
            step = 4U;
        }
    }

    double aspect = (double)height / (double)width;

    double x_min = view->center_x - view->scale;
    double y_max = view->center_y + view->scale * aspect;

    double dx = (2.0 * view->scale) / (double)width;
    double dy = (2.0 * view->scale * aspect) / (double)height;

    for (u32 y = 0; y < height; y += step) {
        double cy = y_max - ((double)y * dy);
        double cx = x_min;

        for (u32 x = 0; x < width; x += step) {
            if (x) {
                cx += dx * (double)step;
            }

            u32 color = 0;

            if (!point_inside_main_body(cx, cy)) {
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

                color = palette_color(iter, max_iter);
            }

            fill_block(fb, x, y, step, color);
        }
    }
}

static int
handle_event(mandelbrot_view_t *view, const ws_input_event_t *event) {
    if (!view || !event) {
        return MBROT_ACTION_NONE;
    }

    if (event->type == INPUT_EVENT_WINDOW_RESIZE) {
        return MBROT_ACTION_RESIZE;
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
        return MBROT_ACTION_RENDER;
    case KBD_MINUS:
    case KBD_KP_MINUS:
        view->scale *= 1.20;
        view_clamp(view);
        return MBROT_ACTION_RENDER;
    case KBD_LEFT:
        view->center_x -= pan_step;
        return MBROT_ACTION_RENDER;
    case KBD_RIGHT:
        view->center_x += pan_step;
        return MBROT_ACTION_RENDER;
    case KBD_UP:
        view->center_y += pan_step;
        return MBROT_ACTION_RENDER;
    case KBD_DOWN:
        view->center_y -= pan_step;
        return MBROT_ACTION_RENDER;
    case KBD_0:
    case KBD_R:
        view_reset(view);
        return MBROT_ACTION_RENDER;
    default:
        return MBROT_ACTION_NONE;
    }
}

int main(void) {
    window_t window = {0};
    if (window_init(&window, 760, 500, "mbrot")) {
        return 1;
    }

    printf("+/- zoom, arrows pan, r reset, q/esc quit\n");

    mandelbrot_view_t view = {0};
    view_reset(&view);

    render_mandelbrot(&window, &view, false);
    if (!flush_frame(&window) && errno != EAGAIN && errno != EINTR) {
        window_deinit(&window);
        return 1;
    }

    bool refine_pending = false;

    while (true) {
        int timeout_ms = refine_pending ? 0 : -1;
        ws_input_event_t event = {0};
        int ret = window_wait_event(&window, &event, timeout_ms);

        if (ret < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            break;
        }

        if (ret == 0) {
            if (refine_pending) {
                render_mandelbrot(&window, &view, false);

                if (!flush_frame(&window)) {
                    if (errno == EAGAIN || errno == EINTR) {
                        refine_pending = true;
                        continue;
                    }
                    break;
                }

                refine_pending = false;
            }
            continue;
        }

        int action = handle_event(&view, &event);

        if (action == MBROT_ACTION_QUIT) {
            break;
        }

        if (action == MBROT_ACTION_NONE) {
            continue;
        }

        if (action == MBROT_ACTION_RESIZE) {
            render_mandelbrot(&window, &view, true);
            refine_pending = true;
        } else {
            render_mandelbrot(&window, &view, false);
            refine_pending = false;
        }

        if (!flush_frame(&window)) {
            if (errno == EAGAIN || errno == EINTR) {
                refine_pending = true;
                continue;
            }
            break;
        }
    }

    window_deinit(&window);
    return 0;
}
