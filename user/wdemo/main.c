#include <draw.h>
#include <input/kbd.h>
#include <stdbool.h>
#include <ui.h>

static bool should_quit(const ws_input_event_t *event) {
    if (!event || event->type != INPUT_EVENT_KEY || !event->action) {
        return false;
    }

    return event->keycode == KBD_ESCAPE || event->keycode == KBD_Q || event->keycode == KBD_ENTER;
}

int main(void) {
    // Open a window
    window_t window = {0};
    if (window_init(&window, 640, 420, "wdemo")) {
        return 1;
    }

    // Get the window's framebuffer
    u32 *pixels = window_buffer(&window);
    if (!pixels) {
        window_deinit(&window);
        return 1;
    }

    // Fill the background with a solid color
    draw_rect(pixels, window.width, window.height, 0, 0, window.width, window.height, 0x00141414U);

    // Draw a filled triangle in the middle of the window
    draw_point_t triangle[3] = {
        {.x = (i32)(window.width / 2), .y = (i32)(window.height / 5)},
        {.x = (i32)(window.width / 4), .y = (i32)((window.height * 4) / 5)},
        {.x = (i32)((window.width * 3) / 4), .y = (i32)((window.height * 4) / 5)},
    };
    draw_polygon(pixels, window.width, window.height, triangle, 3, 0x0000ff00U);

    // Flush the buffer to the window
    if (window_flush(&window)) {
        window_deinit(&window);
        return 1;
    }

    // Wait for the user to press escape, q, or enter
    ws_input_event_t event = {0};
    while (window_wait_event(&window, &event, -1) >= 0) {
        if (should_quit(&event)) {
            break;
        }
    }

    // Clean up and exit
    window_deinit(&window);

    return 0;
}
