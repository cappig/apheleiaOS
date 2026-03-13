#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <input/kbd.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/proc.h>
#include <sys/wait.h>
#include <ui.h>
#include <unistd.h>

#include "pty.h"
#include "screen.h"

static volatile sig_atomic_t term_exit_requested = 0;

enum {
    TERM_EVENT_BATCH = 32,
    TERM_EVENT_BUDGET = 512,
    TERM_SCROLL_STEP = 3,
};

static void term_exit_signal(int signum) {
    (void)signum;
    term_exit_requested = 1;
}

static bool child_alive(pid_t child) {
    if (child <= 0) {
        return false;
    }

    int status = 0;
    pid_t done = waitpid(child, &status, WNOHANG);
    if (!done) {
        return true;
    }

    if (done == child) {
        return false;
    }

    return errno != ECHILD;
}

static void wait_ms(int ms) {
    if (ms <= 0) {
        return;
    }

    (void)poll(NULL, 0, ms);
}

static bool is_pid_dir_name(const char *name) {
    if (!name || !name[0]) {
        return false;
    }

    for (const char *p = name; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }

    return true;
}

static bool session_alive(pid_t sid) {
    if (sid <= 0) {
        return false;
    }

    DIR *dir = opendir("/proc");
    if (!dir) {
        return false;
    }

    bool alive = false;
    struct dirent *ent = NULL;

    while ((ent = readdir(dir)) != NULL) {
        if (!is_pid_dir_name(ent->d_name)) {
            continue;
        }

        char stat_path[80];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent->d_name);

        proc_stat_t stat = {0};
        if (proc_stat_read_path(stat_path, &stat) < 0) {
            continue;
        }

        if (stat.sid != sid || stat.state == PROC_STATE_ZOMBIE) {
            continue;
        }

        alive = true;
        break;
    }

    closedir(dir);
    return alive;
}

static bool master_tty_index(int master_fd, int *tty_index_out) {
    if (master_fd < 0 || !tty_index_out) {
        return false;
    }

    int ptn = -1;
    if (ioctl(master_fd, TIOCGPTN, &ptn) || ptn < 0) {
        return false;
    }

    *tty_index_out = PROC_TTY_PTS(ptn);
    return true;
}

static bool tty_alive(int tty_index) {
    DIR *dir = opendir("/proc");
    if (!dir) {
        return false;
    }

    bool alive = false;
    struct dirent *ent = NULL;

    while ((ent = readdir(dir)) != NULL) {
        if (!is_pid_dir_name(ent->d_name)) {
            continue;
        }

        char stat_path[80];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent->d_name);

        proc_stat_t stat = {0};
        if (proc_stat_read_path(stat_path, &stat) < 0) {
            continue;
        }

        if (stat.tty_index != tty_index || stat.state == PROC_STATE_ZOMBIE) {
            continue;
        }

        alive = true;
        break;
    }

    closedir(dir);
    return alive;
}

static void signal_session(pid_t sid, int signum) {
    if (sid <= 0 || signum <= 0) {
        return;
    }

    DIR *dir = opendir("/proc");
    if (!dir) {
        return;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_pid_dir_name(ent->d_name)) {
            continue;
        }

        char stat_path[80];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent->d_name);

        proc_stat_t stat = {0};
        if (proc_stat_read_path(stat_path, &stat) < 0) {
            continue;
        }

        if (stat.sid != sid || stat.state == PROC_STATE_ZOMBIE) {
            continue;
        }

        kill(stat.pid, signum);
    }

    closedir(dir);
}

static void signal_tty(int tty_index, int signum) {
    if (signum <= 0) {
        return;
    }

    DIR *dir = opendir("/proc");
    if (!dir) {
        return;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_pid_dir_name(ent->d_name)) {
            continue;
        }

        char stat_path[80];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent->d_name);

        proc_stat_t stat = {0};
        if (proc_stat_read_path(stat_path, &stat) < 0) {
            continue;
        }

        if (stat.tty_index != tty_index || stat.state == PROC_STATE_ZOMBIE) {
            continue;
        }

        kill(stat.pid, signum);
    }

    closedir(dir);
}

static bool teardown_targets_alive(int master_fd, pid_t sid) {
    if (sid > 0 && session_alive(sid)) {
        return true;
    }

    int tty_index = 0;
    if (master_tty_index(master_fd, &tty_index) && tty_alive(tty_index)) {
        return true;
    }

    return false;
}

static void signal_teardown_targets(int master_fd, pid_t sid, int signum) {
    if (signum <= 0) {
        return;
    }

    int tty_index = 0;
    if (master_tty_index(master_fd, &tty_index)) {
        signal_tty(tty_index, signum);
    }

    if (sid > 0) {
        signal_session(sid, signum);
    }
}

static void stop_child(int master_fd, pid_t child) {
    pid_t fg_pgrp = 0;
    if (master_fd >= 0 && ioctl(master_fd, TIOCGPGRP, &fg_pgrp) == 0 && fg_pgrp > 0) {
        kill(-fg_pgrp, SIGHUP);
        kill(-fg_pgrp, SIGCONT);
    }

    signal_teardown_targets(master_fd, child, SIGHUP);
    signal_teardown_targets(master_fd, child, SIGCONT);

    if (child > 0) {
        kill(-child, SIGHUP);
        kill(child, SIGHUP);
    }

    for (size_t i = 0; i < 20; i++) {
        if (!teardown_targets_alive(master_fd, child)) {
            return;
        }

        wait_ms(10);
    }

    signal_teardown_targets(master_fd, child, SIGTERM);
    if (child > 0) {
        kill(-child, SIGTERM);
    }

    for (size_t i = 0; i < 20; i++) {
        if (!teardown_targets_alive(master_fd, child)) {
            return;
        }

        wait_ms(10);
    }

    signal_teardown_targets(master_fd, child, SIGKILL);
    if (child > 0) {
        kill(-child, SIGKILL);
    }
}

static bool read_pty(int master_fd) {
    if (master_fd < 0) {
        return false;
    }

    u8 buf[4096];

    for (;;) {
        ssize_t n = read(master_fd, buf, sizeof(buf));

        if (n > 0) {
            term_screen_feed(buf, (size_t)n);
            continue;
        }

        if (!n) {
            return false;
        }

        if (errno == EAGAIN || errno == EINTR) {
            return true;
        }

        return false;
    }
}

static bool sync_screen_size(window_t *window, int master_fd) {
    if (!window || master_fd < 0) {
        return false;
    }

    framebuffer_t *fb = window_buffer(window);
    if (!fb || !fb->pixels || !term_screen_resize(fb)) {
        return false;
    }

    term_set_winsize(
        master_fd,
        term_screen_cols(),
        term_screen_rows(),
        window->width,
        window->height
    );

    return true;
}

static bool is_modifier_key(u32 keycode) {
    return keycode == KBD_LEFT_SHIFT || keycode == KBD_RIGHT_SHIFT ||
           keycode == KBD_LEFT_CTRL || keycode == KBD_RIGHT_CTRL ||
           keycode == KBD_LEFT_ALT || keycode == KBD_RIGHT_ALT ||
           keycode == KBD_LEFT_SUPER || keycode == KBD_RIGHT_SUPER ||
           keycode == KBD_CAPSLOCK || keycode == KBD_NUMLOCK ||
           keycode == KBD_SCRLLOCK;
}

static bool read_window(window_t *window, int master_fd) {
    if (!window || master_fd < 0) {
        return false;
    }

    bool pending_resize = false;
    bool window_closed = false;
    size_t handled = 0;

    while (handled < TERM_EVENT_BUDGET) {
        ws_input_event_t events[TERM_EVENT_BATCH];
        ssize_t n = window_events(window, events, TERM_EVENT_BATCH);

        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                break;
            }

            if (errno == ENOENT) {
                window_closed = true;
            }

            break;
        }

        size_t count = (size_t)n / sizeof(events[0]);
        if (!count) {
            break;
        }

        for (size_t i = 0; i < count; i++) {
            ws_input_event_t *event = &events[i];

            if (
                event->type == INPUT_EVENT_WINDOW_RESIZE &&
                event->width &&
                event->height
            ) {
                pending_resize = true;
                continue;
            }

            if (event->type == INPUT_EVENT_KEY) {
                if (event->action) {
                    size_t page = term_screen_rows();
                    int page_lines = page > 1 ? (int)(page - 1) : 1;

                    if (event->keycode == KBD_PAGEUP) {
                        (void)term_screen_scroll_lines(page_lines);
                        continue;
                    }

                    if (event->keycode == KBD_PAGEDOWN) {
                        (void)term_screen_scroll_lines(-page_lines);
                        continue;
                    }

                    bool shift = (event->modifiers & INPUT_MOD_SHIFT) != 0;
                    if (shift && event->keycode == KBD_UP) {
                        (void)term_screen_scroll_lines(TERM_SCROLL_STEP);
                        continue;
                    }

                    if (shift && event->keycode == KBD_DOWN) {
                        (void)term_screen_scroll_lines(-TERM_SCROLL_STEP);
                        continue;
                    }

                    if (
                        term_screen_scroll_offset() > 0 &&
                        !is_modifier_key(event->keycode)
                    ) {
                        term_screen_scroll_bottom();
                    }
                }

                term_handle_key_event(master_fd, event);
                continue;
            }

            if (event->type == INPUT_EVENT_MOUSE_WHEEL && event->wheel) {
                (void)term_screen_scroll_lines(-event->wheel * TERM_SCROLL_STEP);
            }
        }

        handled += count;

        if (count < TERM_EVENT_BATCH) {
            break;
        }
    }

    if (pending_resize) {
        sync_screen_size(window, master_fd);
    }

    if (window_closed) {
        errno = ENOENT;
        return false;
    }

    return true;
}

int main(void) {
    window_t window = {0};
    if (window_init(&window, 800, 500, "term")) {
        return 1;
    }

    framebuffer_t *fb = window_buffer(&window);
    if (!fb || !fb->pixels) {
        window_deinit(&window);
        return 1;
    }

    if (!term_screen_init(fb)) {
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
        master_fd,
        term_screen_cols(),
        term_screen_rows(),
        window.width,
        window.height
    );
    if (child < 0) {
        close(master_fd);
        window_deinit(&window);
        return 1;
    }

    struct pollfd pfds[2] = {
        {
            .fd = master_fd,
            .events = POLLIN,
            .revents = 0,
        },
        {
            .fd = window.ev_fd,
            .events = POLLIN,
            .revents = 0,
        },
    };

    bool running = true;
    bool pending_flush = false;
    u32 flush_x = 0;
    u32 flush_y = 0;
    u32 flush_w = 0;
    u32 flush_h = 0;

    signal(SIGHUP, term_exit_signal);
    signal(SIGTERM, term_exit_signal);
    signal(SIGINT, term_exit_signal);

    while (running) {
        if (term_exit_requested) {
            break;
        }

        if (!child_alive(child)) {
            break;
        }

        int timeout_ms = pending_flush ? 16 : -1;
        int ready = poll(pfds, 2, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            if (!(pfds[0].revents & POLLIN)) {
                break;
            }
        }

        if (pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            running = false;
            continue;
        }

        if (pfds[1].revents & POLLIN) {
            if (!read_window(&window, master_fd)) {
                if (errno == ENOENT) {
                    running = false;
                    continue;
                }
            }
        }

        if ((pfds[0].revents & POLLIN) || !ready) {
            if (!read_pty(master_fd)) {
                break;
            }
        }

        pfds[0].revents = 0;
        pfds[1].revents = 0;

        if (term_exit_requested) {
            break;
        }

        if (!pending_flush && !term_screen_render_rect(&flush_x, &flush_y, &flush_w, &flush_h)) {
            continue;
        }

        pending_flush = true;

        if (window_flush_rect(&window, flush_x, flush_y, flush_w, flush_h) < 0) {
            if (errno == ENOENT) {
                break;
            }
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            // Keep terminal alive through transient geometry/write races
            pending_flush = false;
            continue;
        }

        pending_flush = false;

        if (term_screen_render_rect(&flush_x, &flush_y, &flush_w, &flush_h)) {
            pending_flush = true;
        }
    }

    stop_child(master_fd, child);

    close(master_fd);
    window_deinit(&window);
    return 0;
}
