#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/wait.h>
#include <ui.h>
#include <unistd.h>

#include "term_pty.h"
#include "term_screen.h"

static bool child_alive(pid_t child) {
    if (child <= 0)
        return false;

    int status = 0;
    pid_t done = waitpid(child, &status, WNOHANG);
    return done != child;
}

static void stop_child(pid_t child) {
    if (child <= 0)
        return;

    kill(-child, SIGHUP);
    kill(child, SIGHUP);
}

static bool read_pty(int master_fd) {
    if (master_fd < 0)
        return false;

    u8 buf[4096];
    bool got_data = false;

    for (;;) {
        ssize_t n = read(master_fd, buf, sizeof(buf));

        if (n > 0) {
            term_screen_feed(buf, (size_t)n);
            got_data = true;
            continue;
        }

        if (!n)
            return got_data;

        if (errno == EAGAIN || errno == EINTR)
            return got_data || (errno == EAGAIN);

        return false;
    }
}

static bool read_window(window_t* window, int master_fd) {
    if (!window || master_fd < 0)
        return false;

    ws_input_event_t events[16];
    ssize_t n = window_events(window, events, sizeof(events) / sizeof(events[0]));

    if (n < 0)
        return errno == EAGAIN || errno == EINTR;

    size_t count = (size_t)n / sizeof(events[0]);
    for (size_t i = 0; i < count; i++) {
        if (events[i].type != INPUT_EVENT_KEY)
            continue;

        term_handle_key_event(master_fd, &events[i]);
    }

    return true;
}

int main(void) {
    window_t window = {0};
    if (window_init(&window, 800, 500, "term"))
        return 1;

    u32* pixels = window_buffer(&window);
    if (!pixels) {
        window_deinit(&window);
        return 1;
    }

    if (!term_screen_init(
            window.width, window.height, pixels, (size_t)window.width * (size_t)window.height
        )) {
        window_deinit(&window);
        return 1;
    }

    if (window_flush(&window) < 0) {
        window_deinit(&window);
        return 1;
    }

    int master_fd = open("/dev/ptmx", O_RDWR | O_NONBLOCK, 0);
    if (master_fd < 0) {
        window_deinit(&window);
        return 1;
    }

    pid_t child = term_spawn_shell(
        master_fd, term_screen_cols(), term_screen_rows(), window.width, window.height
    );
    if (child < 0) {
        close(master_fd);
        window_deinit(&window);
        return 1;
    }

    struct pollfd pfds[2] = {
        {.fd = master_fd, .events = POLLIN, .revents = 0},
        {.fd = window.ev_fd, .events = POLLIN, .revents = 0},
    };

    bool running = true;
    bool pending_flush = false;
    u32 flush_x = 0;
    u32 flush_y = 0;
    u32 flush_w = 0;
    u32 flush_h = 0;

    while (running) {
        if (!child_alive(child))
            break;

        int timeout_ms = pending_flush ? 16 : -1;
        int ready = poll(pfds, 2, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR)
                continue;

            break;
        }

        if ((pfds[0].revents & POLLIN) && !read_pty(master_fd))
            break;

        if (pfds[1].revents & POLLIN) {
            if (!read_window(&window, master_fd)) {
                if (errno == ENOENT) {
                    running = false;
                    continue;
                }

                if (errno != EAGAIN && errno != EINTR)
                    break;
            }
        }

        if (!pending_flush && !term_screen_render_rect(&flush_x, &flush_y, &flush_w, &flush_h))
            continue;

        pending_flush = true;

        if (window_flush_rect(&window, flush_x, flush_y, flush_w, flush_h) < 0) {
            if (errno == ENOENT)
                break;
            if (errno == EAGAIN)
                continue;

            if (errno != EAGAIN)
                break;
        }

        pending_flush = false;
    }

    if (child_alive(child))
        stop_child(child);

    close(master_fd);
    window_deinit(&window);
    return 0;
}
