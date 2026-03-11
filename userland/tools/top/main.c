#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <kv.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/proc.h>
#include <termios.h>
#include <unistd.h>

#define TOP_DEFAULT_DELAY_MS 1000U
#define TOP_DEFAULT_ROWS     24
#define TOP_DEFAULT_COLS     80
#define TOP_COL_PID          5
#define TOP_COL_TTY          5
#define TOP_COL_STAT         4
#define TOP_COL_CORE         4
#define TOP_COL_CPU          6
#define TOP_COL_MEM          6
#define TOP_COL_TIME         5
#define TOP_CPU_MAX_CORES    64
#define TOP_CPU_TEXT_MAX     8192
#define TOP_BAR_TEXT_MAX     160

typedef struct {
    unsigned long long now;
    unsigned long long boot;
    unsigned long long hz;
    unsigned long long ticks;
} top_clock_t;

typedef struct {
    unsigned long long total_kib;
    unsigned long long used_kib;
} top_mem_t;

typedef struct {
    unsigned long long ncpu;
    unsigned long long busy_ticks;
    unsigned long long total_ticks;
    unsigned long long core_busy_ticks[TOP_CPU_MAX_CORES];
    unsigned long long core_total_ticks[TOP_CPU_MAX_CORES];
} top_cpu_t;

typedef struct {
    pid_t pid;
    unsigned long long cpu_time_ms;
} top_prev_proc_t;

typedef struct {
    proc_stat_t stat;
    unsigned long long delta_ms;
    unsigned long long cpu_pct_x100;
} top_proc_t;

typedef struct {
    size_t total;
    size_t running;
    size_t sleeping;
    size_t stopped;
    size_t zombie;
    size_t unknown;
} top_task_count_t;

typedef struct {
    bool active;
    int fd;
    termios_t saved;
} top_tty_state_t;

typedef struct {
    int clock_fd;
    int swap_fd;
    int cpu_fd;
} top_data_fds_t;

static void top_write(const char *text) {
    if (!text) {
        return;
    }

    (void)write(STDOUT_FILENO, text, strlen(text));
}

static void
top_write_line(const char *line, bool interactive, size_t *line_count) {
    if (!line) {
        return;
    }

    if (interactive) {
        top_write("\x1b[2K");
    }

    top_write(line);

    if (line_count) {
        (*line_count)++;
    }
}

static void top_usage(void) {
    static const char usage[] =
        "usage: top [-d delay_ms] [-n samples] [-c] [-p] [-g]\n"
        "  -d delay_ms  refresh delay in milliseconds (default: 1000)\n"
        "  -c           show running core column\n"
        "  -p           show per-core CPU usage summary\n"
        "  -g           show CPU/MEM bar graphs\n"
        "  -n samples   number of updates before exit (default: forever in tty,\n"
        "               one snapshot when not interactive)\n";
    (void)write(STDOUT_FILENO, usage, sizeof(usage) - 1);
}

static bool top_parse_u64(const char *text, unsigned long long *out) {
    if (!text || !text[0] || !out) {
        return false;
    }

    char *end = NULL;
    long long parsed = strtoll(text, &end, 10);
    if (end == text || *end != '\0' || parsed < 0) {
        return false;
    }

    *out = (unsigned long long)parsed;
    return true;
}

static bool top_is_pid_name(const char *name) {
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

static const char *top_state_name(char state) {
    switch (state) {
    case PROC_STATE_RUNNING:
        return "R";
    case PROC_STATE_SLEEPING:
        return "S";
    case PROC_STATE_STOPPED:
        return "T";
    case PROC_STATE_ZOMBIE:
        return "Z";
    default:
        return "?";
    }
}

static int top_state_rank(char state) {
    switch (state) {
    case PROC_STATE_RUNNING:
        return 0;
    case PROC_STATE_SLEEPING:
        return 1;
    case PROC_STATE_STOPPED:
        return 2;
    case PROC_STATE_ZOMBIE:
        return 3;
    default:
        return 4;
    }
}

static const char *
top_tty_name(const proc_stat_t *info, char *buf, size_t buf_len) {
    if (!info) {
        return "??";
    }

    if (PROC_TTY_IS_PTS(info->tty_index)) {
        snprintf(buf, buf_len, "pts%d", PROC_TTY_PTS_INDEX(info->tty_index));
        return buf;
    }

    if (info->tty_index == PROC_TTY_NONE) {
        return "??";
    }

    if (info->tty_index == PROC_TTY_CONSOLE) {
        return "console";
    }

    snprintf(buf, buf_len, "tty%d", info->tty_index - 1);
    return buf;
}

static void
top_format_uptime(unsigned long long sec, char *out, size_t out_len) {
    if (!out || !out_len) {
        return;
    }

    unsigned long long days = sec / 86400ULL;
    unsigned long long rem = sec % 86400ULL;
    unsigned long long hours = rem / 3600ULL;
    rem %= 3600ULL;
    unsigned long long mins = rem / 60ULL;
    unsigned long long secs = rem % 60ULL;

    if (days) {
        snprintf(
            out,
            out_len,
            "%llud %02llu:%02llu:%02llu",
            days,
            hours,
            mins,
            secs
        );
    } else {
        snprintf(out, out_len, "%02llu:%02llu:%02llu", hours, mins, secs);
    }
}

static void top_format_cpu_time(
    unsigned long long total_ms,
    char *out,
    size_t out_len
) {
    if (!out || !out_len) {
        return;
    }

    unsigned long long total_secs = total_ms / 1000ULL;
    unsigned long long hours = total_secs / 3600ULL;
    unsigned long long mins = (total_secs / 60ULL) % 60ULL;
    unsigned long long secs = total_secs % 60ULL;

    if (hours) {
        snprintf(out, out_len, "%llu:%02llu:%02llu", hours, mins, secs);
        return;
    }

    snprintf(out, out_len, "%llu:%02llu", total_secs / 60ULL, secs);
}

static bool top_read_text_fd(int fd, char *text, size_t text_len) {
    if (fd < 0 || !text || text_len < 2) {
        return false;
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        return false;
    }

    return kv_read_fd(fd, text, text_len) > 0;
}

static bool top_read_clock(int fd, top_clock_t *out) {
    if (!out || fd < 0) {
        return false;
    }

    char text[256] = {0};
    if (!top_read_text_fd(fd, text, sizeof(text))) {
        return false;
    }

    return kv_read_u64(text, "now", &out->now) &&
           kv_read_u64(text, "boot", &out->boot) &&
           kv_read_u64(text, "hz", &out->hz) &&
           kv_read_u64(text, "ticks", &out->ticks);
}

static bool top_read_mem(int fd, top_mem_t *out) {
    if (!out || fd < 0) {
        return false;
    }

    char text[256] = {0};
    if (!top_read_text_fd(fd, text, sizeof(text))) {
        return false;
    }

    return kv_read_u64(text, "total_kib", &out->total_kib) &&
           kv_read_u64(text, "used_kib", &out->used_kib);
}

static bool top_read_cpu(int fd, top_cpu_t *out) {
    if (!out || fd < 0) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->ncpu = 1;

    char text[TOP_CPU_TEXT_MAX] = {0};
    if (!top_read_text_fd(fd, text, sizeof(text))) {
        return false;
    }

    bool ok = kv_read_u64(text, "busy_ticks", &out->busy_ticks) &&
              kv_read_u64(text, "total_ticks", &out->total_ticks);
    if (!ok) {
        return false;
    }

    unsigned long long parsed_ncpu = 0;
    if (kv_read_u64(text, "ncpu", &parsed_ncpu) && parsed_ncpu > 0) {
        out->ncpu = parsed_ncpu;
    }

    size_t limit = out->ncpu < TOP_CPU_MAX_CORES ? (size_t)out->ncpu
                                                  : TOP_CPU_MAX_CORES;
    for (size_t i = 0; i < limit; i++) {
        char key_busy[32];
        char key_total[32];
        snprintf(key_busy, sizeof(key_busy), "core%zu_busy_ticks", i);
        snprintf(key_total, sizeof(key_total), "core%zu_total_ticks", i);

        (void)kv_read_u64(text, key_busy, &out->core_busy_ticks[i]);
        (void)kv_read_u64(text, key_total, &out->core_total_ticks[i]);
    }

    return true;
}

static unsigned long long top_cpu_pct_x10(
    unsigned long long now_busy,
    unsigned long long now_total,
    unsigned long long prev_busy,
    unsigned long long prev_total,
    bool have_prev
) {
    if (have_prev && now_total > prev_total) {
        unsigned long long delta_total = now_total - prev_total;
        unsigned long long delta_busy = 0;
        if (now_busy > prev_busy) {
            delta_busy = now_busy - prev_busy;
        }
        if (delta_busy > delta_total) {
            delta_busy = delta_total;
        }
        return delta_total ? ((delta_busy * 1000ULL) / delta_total) : 0;
    }

    if (now_total) {
        return (now_busy * 1000ULL) / now_total;
    }

    return 0;
}

static void top_fill_bar(
    char *out,
    size_t out_len,
    size_t width,
    unsigned long long pct_x10
) {
    if (!out || out_len < 2) {
        return;
    }

    if (!width) {
        out[0] = '\0';
        return;
    }

    if (width >= out_len) {
        width = out_len - 1;
    }

    size_t fill = (size_t)((pct_x10 * width + 500ULL) / 1000ULL);
    if (fill > width) {
        fill = width;
    }

    for (size_t i = 0; i < width; i++) {
        out[i] = i < fill ? '#' : '-';
    }
    out[width] = '\0';
}

static size_t top_render_usage_bars(
    unsigned long long cpu_pct_x10,
    unsigned long long mem_pct_x10,
    size_t cols,
    bool interactive
) {
    char cpu_bar[64];
    char mem_bar[64];
    size_t bar_width = 10;

    if (cols > 44) {
        bar_width = (cols - 30) / 2;
        if (bar_width > 28) {
            bar_width = 28;
        }
    }

    top_fill_bar(cpu_bar, sizeof(cpu_bar), bar_width, cpu_pct_x10);
    top_fill_bar(mem_bar, sizeof(mem_bar), bar_width, mem_pct_x10);

    char line[TOP_BAR_TEXT_MAX];
    snprintf(
        line,
        sizeof(line),
        "CPU [%s] %llu.%1llu%%  MEM [%s] %llu.%1llu%%\n",
        cpu_bar,
        cpu_pct_x10 / 10ULL,
        cpu_pct_x10 % 10ULL,
        mem_bar,
        mem_pct_x10 / 10ULL,
        mem_pct_x10 % 10ULL
    );
    top_write_line(line, interactive, NULL);
    return 1;
}

static size_t top_render_per_core_bars(
    const top_cpu_t *cpu_now,
    const top_cpu_t *cpu_prev,
    size_t cols,
    size_t max_lines,
    bool interactive
) {
    if (!cpu_now || !cpu_now->ncpu || !max_lines) {
        return 0;
    }

    size_t limit = cpu_now->ncpu < TOP_CPU_MAX_CORES ? (size_t)cpu_now->ncpu
                                                      : TOP_CPU_MAX_CORES;
    size_t bar_width = cols > 28 ? (cols - 20) : 8;
    if (bar_width > 44) {
        bar_width = 44;
    }

    size_t printed = 0;
    size_t i = 0;
    for (; i < limit && printed < max_lines; i++) {
        if (limit > max_lines && printed + 1 == max_lines) {
            break;
        }

        unsigned long long pct_x10 = top_cpu_pct_x10(
            cpu_now->core_busy_ticks[i],
            cpu_now->core_total_ticks[i],
            cpu_prev ? cpu_prev->core_busy_ticks[i] : 0,
            cpu_prev ? cpu_prev->core_total_ticks[i] : 0,
            cpu_prev != NULL
        );

        char bar[64];
        top_fill_bar(bar, sizeof(bar), bar_width, pct_x10);

        char line[TOP_BAR_TEXT_MAX];
        snprintf(
            line,
            sizeof(line),
            "cpu%02zu [%s] %llu.%1llu%%\n",
            i,
            bar,
            pct_x10 / 10ULL,
            pct_x10 % 10ULL
        );
        top_write_line(line, interactive, NULL);
        printed++;
    }

    if (i < limit && printed < max_lines) {
        char line[TOP_BAR_TEXT_MAX];
        snprintf(line, sizeof(line), "... %zu more core(s)\n", limit - i);
        top_write_line(line, interactive, NULL);
        printed++;
    }

    return printed;
}

static size_t top_render_per_core_summary(
    const top_cpu_t *cpu_now,
    const top_cpu_t *cpu_prev,
    size_t cols,
    bool interactive
) {
    if (!cpu_now || !cpu_now->ncpu) {
        return 0;
    }

    char line[512];
    size_t limit = cpu_now->ncpu < TOP_CPU_MAX_CORES ? (size_t)cpu_now->ncpu
                                                      : TOP_CPU_MAX_CORES;

    if (cols > sizeof(line) - 2) {
        cols = sizeof(line) - 2;
    }
    if (cols < 40) {
        cols = 40;
    }

    int base = snprintf(line, sizeof(line), "CPU/core:");
    if (base < 0) {
        return 0;
    }
    size_t used = (size_t)base;

    for (size_t i = 0; i < limit; i++) {
        unsigned long long pct_x10 = top_cpu_pct_x10(
            cpu_now->core_busy_ticks[i],
            cpu_now->core_total_ticks[i],
            cpu_prev ? cpu_prev->core_busy_ticks[i] : 0,
            cpu_prev ? cpu_prev->core_total_ticks[i] : 0,
            cpu_prev != NULL
        );

        char item[32];
        int wrote = snprintf(
            item, sizeof(item), " c%zu=%llu.%1llu%%", i, pct_x10 / 10ULL, pct_x10 % 10ULL
        );
        if (wrote <= 0) {
            continue;
        }

        size_t item_len = (size_t)wrote;
        if (used + item_len >= cols || used + item_len >= sizeof(line) - 1) {
            if (used + 4 < cols && used + 4 < sizeof(line) - 1) {
                memcpy(line + used, " ...", 4);
                used += 4;
            }
            break;
        }

        memcpy(line + used, item, item_len);
        used += item_len;
    }

    line[used++] = '\n';
    line[used] = '\0';
    top_write_line(line, interactive, NULL);
    return 1;
}

static bool
top_proc_reserve(top_proc_t **items, size_t *cap, size_t needed) {
    if (!items || !cap) {
        return false;
    }

    if (*cap >= needed) {
        return true;
    }

    size_t new_cap = *cap ? (*cap * 2) : 64;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    top_proc_t *grown = realloc(*items, new_cap * sizeof(top_proc_t));
    if (!grown) {
        return false;
    }

    *items = grown;
    *cap = new_cap;
    return true;
}

static bool
top_prev_reserve(top_prev_proc_t **items, size_t *cap, size_t needed) {
    if (!items || !cap) {
        return false;
    }

    if (*cap >= needed) {
        return true;
    }

    size_t new_cap = *cap ? (*cap * 2) : 64;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    top_prev_proc_t *grown = realloc(*items, new_cap * sizeof(top_prev_proc_t));
    if (!grown) {
        return false;
    }

    *items = grown;
    *cap = new_cap;
    return true;
}

static bool top_read_procs(top_proc_t **out_items, size_t *out_count) {
    if (!out_items || !out_count) {
        return false;
    }

    DIR *dir = opendir("/proc");
    if (!dir) {
        return false;
    }

    top_proc_t *items = NULL;
    size_t cap = 0;
    size_t count = 0;

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (!top_is_pid_name(ent->d_name)) {
            continue;
        }

        char stat_path[80];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent->d_name);

        proc_stat_t stat = {0};
        if (proc_stat_read_path(stat_path, &stat) < 0) {
            continue;
        }

        if (!top_proc_reserve(&items, &cap, count + 1)) {
            free(items);
            closedir(dir);
            return false;
        }

        items[count].stat = stat;
        items[count].delta_ms = 0;
        items[count].cpu_pct_x100 = 0;
        count++;
    }

    closedir(dir);
    *out_items = items;
    *out_count = count;
    return true;
}

static unsigned long long top_prev_cpu_time(
    const top_prev_proc_t *prev,
    size_t prev_count,
    pid_t pid
) {
    if (!prev || !prev_count) {
        return 0;
    }

    for (size_t i = 0; i < prev_count; i++) {
        if (prev[i].pid == pid) {
            return prev[i].cpu_time_ms;
        }
    }

    return 0;
}

static void top_build_task_counts(
    const top_proc_t *items,
    size_t count,
    top_task_count_t *out
) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->total = count;

    for (size_t i = 0; i < count; i++) {
        char state = items[i].stat.state;
        if (state == PROC_STATE_RUNNING) {
            out->running++;
        } else if (state == PROC_STATE_SLEEPING) {
            out->sleeping++;
        } else if (state == PROC_STATE_STOPPED) {
            out->stopped++;
        } else if (state == PROC_STATE_ZOMBIE) {
            out->zombie++;
        } else {
            out->unknown++;
        }
    }
}

static void top_compute_proc_usage(
    top_proc_t *items,
    size_t count,
    const top_prev_proc_t *prev,
    size_t prev_count,
    unsigned long long elapsed_ms
) {
    for (size_t i = 0; i < count; i++) {
        unsigned long long old_cpu =
            top_prev_cpu_time(prev, prev_count, items[i].stat.pid);
        unsigned long long now_cpu = items[i].stat.cpu_time_ms;
        unsigned long long delta = (now_cpu >= old_cpu) ? (now_cpu - old_cpu) : 0;

        items[i].delta_ms = delta;
        if (!elapsed_ms) {
            items[i].cpu_pct_x100 = 0;
            continue;
        }

        unsigned long long pct_x100 = (delta * 10000ULL) / elapsed_ms;
        items[i].cpu_pct_x100 = pct_x100;
    }
}

static int top_proc_cmp(const void *lhs, const void *rhs) {
    const top_proc_t *a = lhs;
    const top_proc_t *b = rhs;

    if (a->cpu_pct_x100 > b->cpu_pct_x100) {
        return -1;
    }
    if (a->cpu_pct_x100 < b->cpu_pct_x100) {
        return 1;
    }

    if (a->delta_ms > b->delta_ms) {
        return -1;
    }
    if (a->delta_ms < b->delta_ms) {
        return 1;
    }

    int a_rank = top_state_rank(a->stat.state);
    int b_rank = top_state_rank(b->stat.state);
    if (a_rank < b_rank) {
        return -1;
    }
    if (a_rank > b_rank) {
        return 1;
    }

    if (a->stat.pid < b->stat.pid) {
        return -1;
    }
    if (a->stat.pid > b->stat.pid) {
        return 1;
    }

    return 0;
}

static bool top_snapshot_prev(
    top_prev_proc_t **prev_items,
    size_t *prev_count,
    const top_proc_t *items,
    size_t count
) {
    if (!prev_items || !prev_count) {
        return false;
    }

    size_t cap = 0;
    top_prev_proc_t *next = NULL;
    if (!top_prev_reserve(&next, &cap, count)) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        next[i].pid = items[i].stat.pid;
        next[i].cpu_time_ms = items[i].stat.cpu_time_ms;
    }

    free(*prev_items);
    *prev_items = next;
    *prev_count = count;
    return true;
}

static void top_update_winsize(size_t *rows, size_t *cols, int input_fd) {
    size_t new_rows = TOP_DEFAULT_ROWS;
    size_t new_cols = TOP_DEFAULT_COLS;
    winsize_t ws = {0};

    bool ok = (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col) ||
              (!ioctl(input_fd, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col) ||
              (!ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col);

    if (ok) {
        new_rows = ws.ws_row;
        new_cols = ws.ws_col;
    }

    if (!new_rows) {
        new_rows = TOP_DEFAULT_ROWS;
    }
    if (!new_cols) {
        new_cols = TOP_DEFAULT_COLS;
    }

    if (rows) {
        *rows = new_rows;
    }
    if (cols) {
        *cols = new_cols;
    }
}

static bool top_enable_tty_mode(top_tty_state_t *tty, int fd) {
    if (!tty || fd < 0 || !isatty(fd)) {
        return false;
    }

    termios_t tos = {0};
    if (tcgetattr(fd, &tos) < 0) {
        return false;
    }

    tty->saved = tos;
    tty->fd = fd;
    tty->active = true;

    tos.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    tos.c_cc[VMIN] = 0;
    tos.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tos) < 0) {
        tty->active = false;
        return false;
    }

    return true;
}

static void top_restore_tty_mode(top_tty_state_t *tty) {
    if (!tty || !tty->active) {
        return;
    }

    (void)tcsetattr(tty->fd, TCSANOW, &tty->saved);
    tty->active = false;
}

static bool top_handle_input(int input_fd, unsigned int timeout_ms) {
    pollfd pfd = {
        .fd = input_fd,
        .events = POLLIN,
        .revents = 0,
    };

    int pr = poll(&pfd, 1, (int)timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) {
            return true;
        }
        return false;
    }

    if (pr == 0 || !(pfd.revents & POLLIN)) {
        return true;
    }

    for (;;) {
        char ch = 0;
        ssize_t n = read(input_fd, &ch, 1);
        if (n == 1) {
            if (ch == 'q' || ch == 'Q' || ch == 3) {
                return false;
            }
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        break;
    }

    return true;
}

static void top_render(
    const top_proc_t *items,
    size_t count,
    const top_task_count_t *tasks,
    const top_clock_t *clock,
    const top_mem_t *mem,
    const top_cpu_t *cpu_now,
    const top_cpu_t *cpu_prev,
    unsigned long long ncpu,
    bool show_core,
    bool show_per_core,
    bool show_bars,
    bool interactive,
    size_t rows,
    size_t cols,
    size_t *prev_lines
) {
    if (cols < 40) {
        cols = 40;
    }
    if (rows < 8) {
        rows = 8;
    }

    unsigned long long uptime_sec = 0;
    if (clock && clock->now >= clock->boot) {
        uptime_sec = clock->now - clock->boot;
    }

    unsigned long long cpu_pct_x10 = top_cpu_pct_x10(
        cpu_now ? cpu_now->busy_ticks : 0,
        cpu_now ? cpu_now->total_ticks : 0,
        cpu_prev ? cpu_prev->busy_ticks : 0,
        cpu_prev ? cpu_prev->total_ticks : 0,
        cpu_prev != NULL
    );

    unsigned long long mem_pct_x10 = 0;
    if (mem && mem->total_kib) {
        mem_pct_x10 = (mem->used_kib * 1000ULL) / mem->total_kib;
    }

    char uptime_buf[32];
    top_format_uptime(uptime_sec, uptime_buf, sizeof(uptime_buf));

    size_t rendered_lines = 0;

    if (interactive) {
        top_write("\x1b[H");
    }

    char line[256];
    snprintf(
        line,
        sizeof(line),
        "top - up %s, %zu tasks, %zu running, %zu sleeping, %zu stopped, %zu zombie\n",
        uptime_buf,
        tasks ? tasks->total : count,
        tasks ? tasks->running : 0,
        tasks ? tasks->sleeping : 0,
        tasks ? tasks->stopped : 0,
        tasks ? tasks->zombie : 0
    );
    top_write_line(line, interactive, &rendered_lines);

    snprintf(
        line,
        sizeof(line),
        "CPU: %llu.%1llu%% busy (%llu cpu)%sMem: %llu/%llu KiB (%llu.%1llu%%)\n",
        cpu_pct_x10 / 10ULL,
        cpu_pct_x10 % 10ULL,
        ncpu ? ncpu : 1ULL,
        cols > 70 ? "  " : " ",
        mem ? mem->used_kib : 0ULL,
        mem ? mem->total_kib : 0ULL,
        mem_pct_x10 / 10ULL,
        mem_pct_x10 % 10ULL
    );
    top_write_line(line, interactive, &rendered_lines);

    size_t header_lines = 2;

    if (show_per_core && !show_bars) {
        size_t extra =
            top_render_per_core_summary(cpu_now, cpu_prev, cols, interactive);
        header_lines += extra;
        rendered_lines += extra;
    }

    if (show_bars) {
        size_t usage_lines =
            top_render_usage_bars(cpu_pct_x10, mem_pct_x10, cols, interactive);
        header_lines += usage_lines;
        rendered_lines += usage_lines;

        if (show_per_core) {
            size_t reserve = header_lines + 3;
            size_t max_core_lines = rows > reserve ? (rows - reserve) : 0;
            header_lines +=
                top_render_per_core_bars(
                    cpu_now,
                    cpu_prev,
                    cols,
                    max_core_lines,
                    interactive
                );
            rendered_lines = header_lines;
        }
    }
    top_write_line("\n", interactive, &rendered_lines);
    header_lines++;

    if (show_core) {
        snprintf(
            line,
            sizeof(line),
            "%-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %s\n",
            TOP_COL_PID,
            "PID",
            TOP_COL_TTY,
            "TTY",
            TOP_COL_STAT,
            "STAT",
            TOP_COL_CORE,
            "CORE",
            TOP_COL_CPU,
            "CPU%",
            TOP_COL_MEM,
            "MEM%",
            TOP_COL_TIME,
            "TIME",
            "COMMAND"
        );
    } else {
        snprintf(
            line,
            sizeof(line),
            "%-*s  %-*s  %-*s  %-*s  %-*s  %-*s  %s\n",
            TOP_COL_PID,
            "PID",
            TOP_COL_TTY,
            "TTY",
            TOP_COL_STAT,
            "STAT",
            TOP_COL_CPU,
            "CPU%",
            TOP_COL_MEM,
            "MEM%",
            TOP_COL_TIME,
            "TIME",
            "COMMAND"
        );
    }
    top_write_line(line, interactive, &rendered_lines);
    header_lines++;

    size_t fixed_width = TOP_COL_PID + TOP_COL_TTY + TOP_COL_STAT + TOP_COL_CPU +
                         TOP_COL_MEM + TOP_COL_TIME + 12;
    if (show_core) {
        fixed_width += TOP_COL_CORE + 2;
    }
    size_t cmd_width = cols > fixed_width ? (cols - fixed_width) : 8;
    size_t available_rows = rows > header_lines ? (rows - header_lines) : 1;
    size_t show = count < available_rows ? count : available_rows;

    for (size_t i = 0; i < show; i++) {
        char tty_buf[12];
        char time_buf[24];
        const top_proc_t *p = &items[i];

        const char *tty =
            top_tty_name(&p->stat, tty_buf, sizeof(tty_buf));
        top_format_cpu_time(p->stat.cpu_time_ms, time_buf, sizeof(time_buf));
        char cpu_buf[16];
        char mem_buf[16];
        char core_buf[8];
        snprintf(
            cpu_buf,
            sizeof(cpu_buf),
            "%llu.%02llu",
            p->cpu_pct_x100 / 100ULL,
            p->cpu_pct_x100 % 100ULL
        );
        unsigned long long mem_pct_x100 = 0;
        if (mem && mem->total_kib) {
            mem_pct_x100 = (p->stat.vm_kib * 10000ULL) / mem->total_kib;
        }
        snprintf(
            mem_buf,
            sizeof(mem_buf),
            "%llu.%02llu",
            mem_pct_x100 / 100ULL,
            mem_pct_x100 % 100ULL
        );
        if (p->stat.core_id >= 0) {
            snprintf(core_buf, sizeof(core_buf), "%d", p->stat.core_id);
        } else {
            snprintf(core_buf, sizeof(core_buf), "-");
        }

        const char *name = p->stat.name[0] ? p->stat.name : "thread";
        if (show_core) {
            snprintf(
                line,
                sizeof(line),
                "%-*lld  %-*.*s  %-*.*s  %-*.*s  %-*.*s  %-*.*s  %-*.*s  %.*s\n",
                TOP_COL_PID,
                (long long)p->stat.pid,
                TOP_COL_TTY,
                TOP_COL_TTY,
                tty,
                TOP_COL_STAT,
                TOP_COL_STAT,
                top_state_name(p->stat.state),
                TOP_COL_CORE,
                TOP_COL_CORE,
                core_buf,
                TOP_COL_CPU,
                TOP_COL_CPU,
                cpu_buf,
                TOP_COL_MEM,
                TOP_COL_MEM,
                mem_buf,
                TOP_COL_TIME,
                TOP_COL_TIME,
                time_buf,
                (int)cmd_width,
                name
            );
        } else {
            snprintf(
                line,
                sizeof(line),
                "%-*lld  %-*.*s  %-*.*s  %-*.*s  %-*.*s  %-*.*s  %.*s\n",
                TOP_COL_PID,
                (long long)p->stat.pid,
                TOP_COL_TTY,
                TOP_COL_TTY,
                tty,
                TOP_COL_STAT,
                TOP_COL_STAT,
                top_state_name(p->stat.state),
                TOP_COL_CPU,
                TOP_COL_CPU,
                cpu_buf,
                TOP_COL_MEM,
                TOP_COL_MEM,
                mem_buf,
                TOP_COL_TIME,
                TOP_COL_TIME,
                time_buf,
                (int)cmd_width,
                name
            );
        }
        top_write_line(line, interactive, &rendered_lines);
    }

    if (interactive && prev_lines) {
        while (rendered_lines < *prev_lines) {
            top_write_line("\n", true, &rendered_lines);
        }
    }

    if (prev_lines) {
        *prev_lines = rendered_lines;
    }
}

int main(int argc, char **argv) {
    unsigned int delay_ms = TOP_DEFAULT_DELAY_MS;
    unsigned long long max_samples = 0;
    bool show_core = false;
    bool show_per_core = false;
    bool show_bars = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            top_usage();
            return 0;
        }

        if (!strcmp(argv[i], "-d")) {
            if (i + 1 >= argc) {
                top_usage();
                return 1;
            }

            unsigned long long parsed = 0;
            if (!top_parse_u64(argv[i + 1], &parsed) || !parsed) {
                top_usage();
                return 1;
            }

            if (parsed > 60000ULL) {
                parsed = 60000ULL;
            }

            delay_ms = (unsigned int)parsed;
            i++;
            continue;
        }

        if (!strcmp(argv[i], "-n")) {
            if (i + 1 >= argc) {
                top_usage();
                return 1;
            }

            unsigned long long parsed = 0;
            if (!top_parse_u64(argv[i + 1], &parsed) || !parsed) {
                top_usage();
                return 1;
            }

            max_samples = parsed;
            i++;
            continue;
        }

        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--core")) {
            show_core = true;
            continue;
        }

        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--per-core")) {
            show_per_core = true;
            continue;
        }

        if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--bars")) {
            show_bars = true;
            continue;
        }

        top_usage();
        return 1;
    }

    int input_fd = STDIN_FILENO;
    bool close_input = false;
    bool interactive = isatty(STDOUT_FILENO) != 0;

    if (interactive && !isatty(input_fd)) {
        input_fd = open("/dev/tty", O_RDONLY, 0);
        if (input_fd >= 0) {
            close_input = true;
        } else {
            interactive = false;
        }
    }

    if (!interactive && !max_samples) {
        max_samples = 1;
    }

    top_tty_state_t tty = {0};
    if (interactive) {
        (void)top_enable_tty_mode(&tty, input_fd);
        top_write("\x1b[?25l\x1b[H\x1b[2J");
    }

    top_prev_proc_t *prev = NULL;
    size_t prev_count = 0;

    top_cpu_t prev_cpu = {0};
    bool have_prev_cpu = false;

    top_clock_t prev_clock = {0};
    bool have_prev_clock = false;
    size_t prev_render_lines = 0;

    top_data_fds_t data_fds = {
        .clock_fd = open("/dev/clock", O_RDONLY, 0),
        .swap_fd = open("/dev/swap", O_RDONLY, 0),
        .cpu_fd = open("/dev/cpu", O_RDONLY, 0),
    };

    unsigned long long ncpu = 1;

    unsigned long long sample_index = 0;
    int rc = 0;

    for (;;) {
        top_clock_t clock = {0};
        bool have_clock = top_read_clock(data_fds.clock_fd, &clock);

        top_mem_t mem = {0};
        bool have_mem = top_read_mem(data_fds.swap_fd, &mem);

        top_cpu_t cpu = {0};
        bool have_cpu = top_read_cpu(data_fds.cpu_fd, &cpu);
        if (have_cpu && cpu.ncpu > 0) {
            ncpu = cpu.ncpu;
        }

        top_proc_t *procs = NULL;
        size_t proc_count = 0;
        if (!top_read_procs(&procs, &proc_count)) {
            procs = NULL;
            proc_count = 0;
        }

        unsigned long long elapsed_ms = delay_ms;
        if (
            have_clock &&
            have_prev_clock &&
            clock.hz &&
            clock.ticks >= prev_clock.ticks
        ) {
            unsigned long long delta_ticks = clock.ticks - prev_clock.ticks;
            elapsed_ms = (delta_ticks * 1000ULL) / clock.hz;
            if (!elapsed_ms) {
                elapsed_ms = 1;
            }
        }

        top_compute_proc_usage(
            procs,
            proc_count,
            prev,
            prev_count,
            elapsed_ms
        );
        qsort(procs, proc_count, sizeof(*procs), top_proc_cmp);

        top_task_count_t tasks = {0};
        top_build_task_counts(procs, proc_count, &tasks);

        size_t rows = TOP_DEFAULT_ROWS;
        size_t cols = TOP_DEFAULT_COLS;
        top_update_winsize(&rows, &cols, input_fd);

        top_render(
            procs,
            proc_count,
            &tasks,
            have_clock ? &clock : NULL,
            have_mem ? &mem : NULL,
            have_cpu ? &cpu : NULL,
            have_prev_cpu ? &prev_cpu : NULL,
            ncpu,
            show_core,
            show_per_core,
            show_bars,
            interactive,
            rows,
            cols,
            &prev_render_lines
        );

        if (!top_snapshot_prev(&prev, &prev_count, procs, proc_count)) {
            free(procs);
            rc = 1;
            break;
        }
        free(procs);

        if (have_cpu) {
            prev_cpu = cpu;
            have_prev_cpu = true;
        }

        if (have_clock) {
            prev_clock = clock;
            have_prev_clock = true;
        }

        sample_index++;
        if (max_samples && sample_index >= max_samples) {
            break;
        }

        if (!interactive) {
            break;
        }

        if (!top_handle_input(input_fd, delay_ms)) {
            break;
        }
    }

    top_restore_tty_mode(&tty);
    if (interactive) {
        top_write("\x1b[?25h\x1b[0m\r\n");
    }

    if (close_input && input_fd >= 0) {
        close(input_fd);
    }

    if (data_fds.clock_fd >= 0) {
        close(data_fds.clock_fd);
    }
    if (data_fds.swap_fd >= 0) {
        close(data_fds.swap_fd);
    }
    if (data_fds.cpu_fd >= 0) {
        close(data_fds.cpu_fd);
    }

    free(prev);
    return rc;
}
