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

static void view_reset(mandelbrot_view_t* view) {
    if (!view)
        return;

    view->center_x = -0.5;
    view->center_y = 0.0;
    view->scale = 1.6;
}

static void view_clamp(mandelbrot_view_t* view) {
    if (!view)
        return;

    if (view->scale < 0.000000000001)
        view->scale = 0.000000000001;

    if (view->scale > 8.0)
        view->scale = 8.0;
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
    if (iter >= max_iter)
        return 0x00000000U;

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

    if (q * (q + x) <= 0.25 * y2)
        return true;

    double bx = cx + 1.0;
    if (bx * bx + y2 <= 0.0625)
        return true;

    return false;
}

static void fill_block(
    u32* pixels,
    u32 width,
    u32 height,
    u32 x,
    u32 y,
    u32 step,
    u32 color
) {
    if (!pixels || !width || !height || !step)
        return;

    u32 y_end = y + step;
    u32 x_end = x + step;

    if (y_end > height)
        y_end = height;

    if (x_end > width)
        x_end = width;

    for (u32 yy = y; yy < y_end; yy++) {
        size_t row = (size_t)yy * width;
        for (u32 xx = x; xx < x_end; xx++)
            pixels[row + xx] = color;
    }
}

static void render_mandelbrot(window_t* window, const mandelbrot_view_t* view) {
    if (!window || !view)
        return;

    u32* pixels = window_buffer(window);
    if (!pixels)
        return;

    u32 width = window->width;
    u32 height = window->height;
    if (!width || !height)
        return;

    u32 max_iter = compute_max_iter(view->scale);
    u32 step = (width * height >= 260000U) ? 2U : 1U;

    double aspect = (double)height / (double)width;
    double x_min = view->center_x - view->scale;
    double y_max = view->center_y + view->scale * aspect;
    double dx = (2.0 * view->scale) / (double)width;
    double dy = (2.0 * view->scale * aspect) / (double)height;

    for (u32 y = 0; y < height; y += step) {
        double cy = y_max - ((double)y * dy);
        double cx = x_min;

        for (u32 x = 0; x < width; x += step) {
            if (x)
                cx += dx * (double)step;

            u32 color = 0;

            if (!point_inside_main_body(cx, cy)) {
                double zx = 0.0;
                double zy = 0.0;
                u32 iter = 0;

                while (iter < max_iter) {
                    double zx2 = zx * zx;
                    double zy2 = zy * zy;

                    if (zx2 + zy2 > 4.0)
                        break;

                    double zxy = zx * zy;
                    zy = zxy + zxy + cy;
                    zx = zx2 - zy2 + cx;
                    iter++;
                }

                color = palette_color(iter, max_iter);
            }

            fill_block(pixels, width, height, x, y, step, color);
        }
    }
}

static int handle_event(mandelbrot_view_t* view, const ws_input_event_t* event) {
    if (!view || !event)
        return 0;

    if (event->type != INPUT_EVENT_KEY || !event->action)
        return 0;

    double pan_step = view->scale * 0.18;

    switch (event->keycode) {
    case KBD_ESCAPE:
    case KBD_Q:
        return -1;
    case KBD_EQUALS:
    case KBD_KP_PLUS:
        view->scale *= 0.84;
        view_clamp(view);
        return 1;
    case KBD_MINUS:
    case KBD_KP_MINUS:
        view->scale *= 1.20;
        view_clamp(view);
        return 1;
    case KBD_LEFT:
        view->center_x -= pan_step;
        return 1;
    case KBD_RIGHT:
        view->center_x += pan_step;
        return 1;
    case KBD_UP:
        view->center_y += pan_step;
        return 1;
    case KBD_DOWN:
        view->center_y -= pan_step;
        return 1;
    case KBD_0:
    case KBD_R:
        view_reset(view);
        return 1;
    default:
        return 0;
    }
}

int main(void) {
    window_t window = {0};
    if (window_init(&window, 760, 500, "mbrot"))
        return 1;

    printf("+/- zoom, arrows pan, r reset, q/esc quit\n");

    mandelbrot_view_t view = {0};
    view_reset(&view);

    render_mandelbrot(&window, &view);
    if (window_flush(&window)) {
        window_deinit(&window);
        return 1;
    }

    while (true) {
        ws_input_event_t event = {0};
        int ret = window_wait_event(&window, &event, -1);

        if (ret < 0)
            break;

        if (ret == 0)
            continue;

        int action = handle_event(&view, &event);

        if (action < 0)
            break;

        if (!action)
            continue;

        render_mandelbrot(&window, &view);
        if (window_flush(&window))
            break;
    }

    window_deinit(&window);
    return 0;
}
