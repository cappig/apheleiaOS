#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <input/kbd.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/proc.h>
#include <sys/wait.h>
#include <ui.h>
#include <unistd.h>

#include "pty.h"
#include "screen.h"

static volatile sig_atomic_t exit_requested = 0;

enum {
    TERM_EVENT_BATCH = 32,
    TERM_EVENT_BUDGET = 512,
    TERM_SCROLL_STEP = 3,
};

static void term_exit_signal(int signum) {
    (void)signum;
    exit_requested = 1;
}

static void term_log_errno(const char *message) {
    int saved = errno;
    if (!saved) {
        saved = EIO;
    }

    fprintf(stderr, "term: %s (%d: %s)\n", message ? message : "error", saved, strerror(saved));
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
        if (WIFEXITED(status)) {
            fprintf(stderr, "term: child %ld exited with status %d\n", (long)child, WEXITSTATUS(status));
        } else {
            fprintf(stderr, "term: child %ld exited with raw status %#x\n", (long)child, status);
        }
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

        proc_stat_t stat = { 0 };
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

        proc_stat_t stat = { 0 };
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

        proc_stat_t stat = { 0 };
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

        proc_stat_t stat = { 0 };
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

static bool term_targets_alive(int master_fd, pid_t sid) {
    if (sid > 0 && session_alive(sid)) {
        return true;
    }

    int tty_index = 0;
    if (master_tty_index(master_fd, &tty_index) && tty_alive(tty_index)) {
        return true;
    }

    return false;
}

static void signal_targets(int master_fd, pid_t sid, int signum) {
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

    signal_targets(master_fd, child, SIGHUP);
    signal_targets(master_fd, child, SIGCONT);

    if (child > 0) {
        kill(-child, SIGHUP);
        kill(child, SIGHUP);
    }

    for (size_t i = 0; i < 20; i++) {
        if (!term_targets_alive(master_fd, child)) {
            return;
        }

        wait_ms(10);
    }

    signal_targets(master_fd, child, SIGTERM);
    if (child > 0) {
        kill(-child, SIGTERM);
    }

    for (size_t i = 0; i < 20; i++) {
        if (!term_targets_alive(master_fd, child)) {
            return;
        }

        wait_ms(10);
    }

    signal_targets(master_fd, child, SIGKILL);
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

    term_set_winsize(master_fd, term_screen_cols(), term_screen_rows(), window->width, window->height);

    return true;
}

static bool is_modifier_key(u32 keycode) {
    switch (keycode) {
    case KBD_LEFT_SHIFT:
    case KBD_RIGHT_SHIFT:
    case KBD_LEFT_CTRL:
    case KBD_RIGHT_CTRL:
    case KBD_LEFT_ALT:
    case KBD_RIGHT_ALT:
    case KBD_LEFT_SUPER:
    case KBD_RIGHT_SUPER:
    case KBD_CAPSLOCK:
    case KBD_NUMLOCK:
    case KBD_SCRLLOCK:
        return true;
    default:
        return false;
    }
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

            if (event->type == INPUT_EVENT_WINDOW_RESIZE && event->width && event->height) {
                pending_resize = true;
                continue;
            }

            if (event->type == INPUT_EVENT_KEY) {
                if (event->action) {
                    size_t page = term_screen_rows();
                    int page_lines = page > 1 ? (int)(page - 1) : 1;

                    if (event->keycode == KBD_PAGEUP) {
                        (void)term_screen_scroll(page_lines);
                        continue;
                    }

                    if (event->keycode == KBD_PAGEDOWN) {
                        (void)term_screen_scroll(-page_lines);
                        continue;
                    }

                    bool shift = (event->modifiers & INPUT_MOD_SHIFT) != 0;
                    if (shift && event->keycode == KBD_UP) {
                        (void)term_screen_scroll(TERM_SCROLL_STEP);
                        continue;
                    }

                    if (shift && event->keycode == KBD_DOWN) {
                        (void)term_screen_scroll(-TERM_SCROLL_STEP);
                        continue;
                    }

                    if (term_screen_scroll_pos() > 0 && !is_modifier_key(event->keycode)) {
                        term_screen_scroll_end();
                    }
                }

                term_handle_key_event(master_fd, event);
                continue;
            }

            if (event->type == INPUT_EVENT_MOUSE_WHEEL && event->wheel) {
                (void)term_screen_scroll(-event->wheel * TERM_SCROLL_STEP);
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

typedef struct {
    window_t window;
    int master_fd;
    pid_t child;
    struct pollfd pfds[2];
    bool pending_flush;
    u32 flush_x;
    u32 flush_y;
    u32 flush_w;
    u32 flush_h;
} term_app_t;

static void term_app_deinit(term_app_t *app) {
    if (app->master_fd >= 0 && app->child > 0) {
        stop_child(app->master_fd, app->child);
    }

    if (app->master_fd >= 0) {
        close(app->master_fd);
    }

    window_deinit(&app->window);
}

static bool term_app_init(term_app_t *app) {
    memset(app, 0, sizeof(*app));
    app->master_fd = -1;

    ws_hints_t hints = {
        .min_width = 320,
        .min_height = 160,
    };
    if (window_init_ex(&app->window, 800, 500, "term", &hints)) {
        term_log_errno("failed to create window");
        return false;
    }

    framebuffer_t *fb = window_buffer(&app->window);
    if (!fb || !fb->pixels) {
        term_log_errno("failed to acquire window framebuffer");
        return false;
    }

    if (!term_screen_init(fb)) {
        term_log_errno("failed to initialize terminal screen");
        return false;
    }

    if (window_flush(&app->window) < 0) {
        term_log_errno("failed to flush initial frame");
        return false;
    }

    app->master_fd = open("/dev/ptmx", O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
    if (app->master_fd < 0) {
        term_log_errno("failed to open /dev/ptmx");
        return false;
    }

    app->child = term_spawn_shell(
        app->master_fd,
        term_screen_cols(),
        term_screen_rows(),
        app->window.width,
        app->window.height
    );
    if (app->child < 0) {
        term_log_errno("failed to spawn shell");
        return false;
    }

    app->pfds[0] = (struct pollfd){
        .fd = app->master_fd,
        .events = POLLIN,
        .revents = 0,
    };
    app->pfds[1] = (struct pollfd){
        .fd = app->window.ev_fd,
        .events = POLLIN,
        .revents = 0,
    };

    return true;
}

static bool term_handle_events(term_app_t *app, int ready) {
    if (app->pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        if (!(app->pfds[0].revents & POLLIN)) {
            return false;
        }
    }

    if (app->pfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        return false;
    }

    if (app->pfds[1].revents & POLLIN) {
        if (!read_window(&app->window, app->master_fd) && errno == ENOENT) {
            return false;
        }
    }

    if ((app->pfds[0].revents & POLLIN) || !ready) {
        if (!read_pty(app->master_fd)) {
            return false;
        }
    }

    app->pfds[0].revents = 0;
    app->pfds[1].revents = 0;
    return true;
}

static bool term_flush(term_app_t *app) {
    if (!app->pending_flush) {
        bool dirty = term_screen_render_rect(&app->flush_x, &app->flush_y, &app->flush_w, &app->flush_h);
        if (!dirty) {
            return true;
        }

        app->pending_flush = true;
    }

    if (window_flush_rect(&app->window, app->flush_x, app->flush_y, app->flush_w, app->flush_h) < 0) {
        if (errno == ENOENT) {
            return false;
        }

        if (errno == EAGAIN || errno == EINTR) {
            return true;
        }

        // keep terminal alive through transient geometry/write races
        app->pending_flush = false;
        return true;
    }

    app->pending_flush = term_screen_render_rect(&app->flush_x, &app->flush_y, &app->flush_w, &app->flush_h);
    return true;
}

int main(void) {
    term_app_t app = { 0 };
    if (!term_app_init(&app)) {
        term_app_deinit(&app);
        return 1;
    }

    signal(SIGHUP, term_exit_signal);
    signal(SIGTERM, term_exit_signal);
    signal(SIGINT, term_exit_signal);

    while (!exit_requested && child_alive(app.child)) {
        int timeout_ms = app.pending_flush ? 16 : -1;
        int ready = poll(app.pfds, 2, timeout_ms);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (!term_handle_events(&app, ready)) {
            break;
        }

        if (!exit_requested && !term_flush(&app)) {
            break;
        }
    }

    term_app_deinit(&app);
    return 0;
}
