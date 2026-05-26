#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/proc.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "complete.h"
#include "input.h"

#define SH_ENV_MAX       32
#define SH_ENV_KEY_MAX   32
#define SH_ENV_VAL_MAX   128
#define SH_EXPAND_MAX    256
#define SH_MAX_ARGS      16
#define SH_MAX_TOKENS    64
#define SH_MAX_STAGES    8
#define SH_MAX_CLAUSES   16
#define SH_MAX_JOBS      16
#define SH_CMD_MAX       128
#define SH_LINE_MAX      SH_INPUT_LINE_MAX
#define SH_ENV_ENTRY_MAX (SH_ENV_KEY_MAX + SH_ENV_VAL_MAX + 2)

#define SH_C_RESET "\x1b[0m"
#define SH_C_DIM   "\x1b[90m"
#define SH_C_RED   "\x1b[31m"
#define SH_C_GREEN "\x1b[32m"
#define SH_C_BLUE  "\x1b[34m"
#define SH_C_CYAN  "\x1b[36m"

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
} job_state_t;

typedef struct {
    int id;
    pid_t pid;
    job_state_t state;
    char cmd[SH_CMD_MAX];
} job_t;

typedef struct {
    char key[SH_ENV_KEY_MAX];
    char value[SH_ENV_VAL_MAX];
} sh_env_t;

typedef struct {
    char *argv[SH_MAX_ARGS];
    int argc;
    char *in_path;
    char *out_path;
    bool out_append;
} sh_stage_t;

typedef struct {
    bool stopped;
    int exit_status;
} sh_wait_result_t;

typedef enum {
    SH_CHAIN_ALWAYS,
    SH_CHAIN_AND,
    SH_CHAIN_OR,
} sh_chain_op_t;

typedef struct {
    char *text;
    sh_chain_op_t op;
} sh_clause_t;

typedef struct {
    struct timespec real;
    struct tms cpu;
    bool have_real;
    bool have_cpu;
} sh_time_t;

typedef struct {
    volatile sig_atomic_t got_sigint;
    volatile sig_atomic_t got_sigwinch;
    volatile sig_atomic_t got_sigchld;

    sh_env_t env[SH_ENV_MAX];
    size_t env_count;

    job_t jobs[SH_MAX_JOBS];
    size_t job_count;
    int next_job_id;

    pid_t pgid;
    int last_status;
    pid_t last_bg_pid;
    bool interactive;
    bool color;
    bool no_color;
} sh_state_t;

static sh_state_t sh = {
    .next_job_id = 1,
};

static void sh_printf(const char *format, ...) {
    if (!format) {
        return;
    }

    char line[SH_LINE_MAX];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    io_write_str(line);
}

static const char *sh_color(const char *code) {
    if (!sh.color || sh.no_color || !code) {
        return "";
    }

    return code;
}

static void sh_errorf(const char *format, ...) {
    if (!format) {
        return;
    }

    char line[SH_LINE_MAX];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    if (sh.color && !sh.no_color) {
        io_write_str(SH_C_RED);
    }

    size_t len = strlen(line);
    bool newline = len > 0 && line[len - 1] == '\n';
    if (newline) {
        write(STDOUT_FILENO, line, len - 1);
    } else {
        io_write_str(line);
    }

    if (sh.color && !sh.no_color) {
        io_write_str(SH_C_RESET);
    }

    if (newline) {
        io_write_str("\n");
    }
}

static bool sh_clock_now(struct timespec *out) {
    return out && clock_gettime(CLOCK_MONOTONIC, out) == 0;
}

static long long sh_elapsed_ms(const struct timespec *start, const struct timespec *end) {
    if (!start || !end) {
        return 0;
    }

    long long sec = (long long)(end->tv_sec - start->tv_sec);
    long long nsec = (long long)(end->tv_nsec - start->tv_nsec);

    return sec * 1000 + nsec / 1000000;
}

static bool sh_time_begin(sh_time_t *time) {
    if (!time) {
        return false;
    }

    memset(time, 0, sizeof(*time));
    time->have_real = sh_clock_now(&time->real);
    time->have_cpu = times(&time->cpu) != (clock_t)-1;

    return time->have_real || time->have_cpu;
}

static long long sh_ticks_ms(clock_t ticks) {
    if (ticks <= 0) {
        return 0;
    }

    long ticks_per_sec = sysconf(_SC_CLK_TCK);
    if (ticks_per_sec <= 0) {
        ticks_per_sec = 100;
    }

    return ((long long)ticks * 1000LL) / ticks_per_sec;
}

static clock_t sh_tick_delta(clock_t start, clock_t end) {
    return end > start ? end - start : 0;
}

static void sh_print_seconds(const char *label, long long ms) {
    if (ms < 0) {
        ms = 0;
    }

    fprintf(stderr, "%s %lld.%03llds\n", label, ms / 1000, ms % 1000);
}

static void sh_print_time(const sh_time_t *start) {
    if (!start) {
        return;
    }

    struct timespec end = { 0 };
    long long real_ms = 0;

    if (start->have_real && sh_clock_now(&end)) {
        real_ms = sh_elapsed_ms(&start->real, &end);
    }

    long long user_ms = 0;
    long long sys_ms = 0;

    if (start->have_cpu) {
        struct tms cpu = { 0 };

        if (times(&cpu) != (clock_t)-1) {
            clock_t user_ticks = sh_tick_delta(start->cpu.tms_utime, cpu.tms_utime);
            clock_t child_user_ticks = sh_tick_delta(start->cpu.tms_cutime, cpu.tms_cutime);
            clock_t sys_ticks = sh_tick_delta(start->cpu.tms_stime, cpu.tms_stime);
            clock_t child_sys_ticks = sh_tick_delta(start->cpu.tms_cstime, cpu.tms_cstime);

            user_ms = sh_ticks_ms(user_ticks + child_user_ticks);
            sys_ms = sh_ticks_ms(sys_ticks + child_sys_ticks);
        }
    }

    sh_print_seconds("real", real_ms);
    sh_print_seconds("user", user_ms);
    sh_print_seconds("sys ", sys_ms);
}

static void sigint_handler(int signum) {
    (void)signum;
    sh.got_sigint = 1;
}

static void sigwinch_handler(int signum) {
    (void)signum;
    sh.got_sigwinch = 1;
}

static void sigchld_handler(int signum) {
    (void)signum;
    sh.got_sigchld = 1;
}

static void tty_set_pgrp(pid_t pid) {
    if (pid <= 0) {
        return;
    }

    ioctl(STDIN_FILENO, TIOCSPGRP, &pid);
}

static job_t *job_find_by_id(int id) {
    for (size_t i = 0; i < sh.job_count; i++) {
        if (sh.jobs[i].id == id) {
            return &sh.jobs[i];
        }
    }

    return NULL;
}

static void job_remove_index(size_t index) {
    if (index >= sh.job_count) {
        return;
    }

    for (size_t i = index; i + 1 < sh.job_count; i++) {
        sh.jobs[i] = sh.jobs[i + 1];
    }

    memset(&sh.jobs[sh.job_count - 1], 0, sizeof(sh.jobs[sh.job_count - 1]));
    sh.job_count--;
}

static job_t *job_add(pid_t pid, const char *cmd, job_state_t state) {
    if (sh.job_count >= SH_MAX_JOBS) {
        return NULL;
    }

    job_t *job = &sh.jobs[sh.job_count++];

    memset(job, 0, sizeof(*job));
    job->id = sh.next_job_id++;
    job->pid = pid;
    job->state = state;
    snprintf(job->cmd, sizeof(job->cmd), "%s", cmd ? cmd : "");

    return job;
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

static bool pgrp_state(pid_t pgid, bool *stopped_out) {
    if (stopped_out) {
        *stopped_out = false;
    }

    if (pgid <= 0) {
        return false;
    }

    DIR *dir = opendir("/proc");
    if (!dir) {
        return false;
    }

    bool any_alive = false;
    bool any_running = false;
    bool any_stopped = false;

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

        if (stat.pgid != pgid) {
            continue;
        }

        if (stat.state == PROC_STATE_ZOMBIE) {
            continue;
        }

        any_alive = true;

        if (stat.state == PROC_STATE_STOPPED) {
            any_stopped = true;
        } else {
            any_running = true;
        }
    }

    closedir(dir);

    if (stopped_out && any_alive && any_stopped && !any_running) {
        *stopped_out = true;
    }

    return any_alive;
}

static void reap_jobs(bool report) {
    for (size_t i = 0; i < sh.job_count;) {
        job_t *job = &sh.jobs[i];
        bool stopped = false;
        bool alive = pgrp_state(job->pid, &stopped);

        if (!alive) {
            int status = 0;

            while (waitpid(-job->pid, &status, WNOHANG) > 0) {
                ;
            }

            if (report) {
                sh_printf("[%d] Done    %s\n", job->id, job->cmd);
            }

            job_remove_index(i);
            continue;
        }

        job->state = stopped ? JOB_STOPPED : JOB_RUNNING;

        i++;
    }
}

static void reap_jobs_signal(void) {
    reap_jobs(false);
}

static void print_jobs(void) {
    reap_jobs(false);

    for (size_t i = 0; i < sh.job_count; i++) {
        job_t *job = &sh.jobs[i];
        const char *state = job->state == JOB_STOPPED ? "Stopped" : "Running";
        const char *state_color = job->state == JOB_STOPPED ? SH_C_BLUE : SH_C_GREEN;

        sh_printf(
            "[%d] %s%s%s  %s\n",
            job->id,
            sh_color(state_color),
            state,
            sh_color(SH_C_RESET),
            job->cmd
        );
    }
}

static int parse_job_id(const char *arg) {
    if (!arg || !arg[0]) {
        return -1;
    }

    if (arg[0] == '%') {
        arg++;
    }

    int id = 0;
    while (*arg) {
        if (!isdigit((unsigned char)*arg)) {
            return -1;
        }
        id = id * 10 + (*arg - '0');
        arg++;
    }

    return id > 0 ? id : -1;
}

static sh_wait_result_t wait_foreground_pgrp(pid_t pgid, pid_t tracked_pid) {
    sh_wait_result_t result = {
        .stopped = false,
        .exit_status = 0,
    };

    if (pgid <= 0) {
        return result;
    }

    bool have_tracked_status = (tracked_pid <= 0);

    for (;;) {
        int status = 0;
        pid_t waited = waitpid(-pgid, &status, WUNTRACED);

        if (waited > 0) {
            if (WIFSTOPPED(status)) {
                result.stopped = true;
                return result;
            }

            if (tracked_pid > 0 && waited == tracked_pid) {
                have_tracked_status = true;

                if (WIFEXITED(status)) {
                    result.exit_status = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.exit_status = 128 + WTERMSIG(status);
                } else {
                    result.exit_status = 1;
                }
            }

            continue;
        }

        if (errno == EINTR) {
            reap_jobs(false);
            continue;
        }

        if (errno == ECHILD) {
            if (!have_tracked_status) {
                result.exit_status = 1;
            }
            return result;
        }

        if (!have_tracked_status) {
            result.exit_status = 1;
        }

        return result;
    }
}

static job_t *select_job(int argc, char *const argv[], const char *cmd, size_t *index_out) {
    if (!sh.job_count) {
        sh_printf("%s: no jobs\n", cmd);
        return NULL;
    }

    job_t *job = NULL;
    if (argc < 2) {
        job = &sh.jobs[sh.job_count - 1];
    } else {
        job = job_find_by_id(parse_job_id(argv[1]));
    }

    if (!job) {
        sh_printf("%s: no such job\n", cmd);
        return NULL;
    }

    if (index_out) {
        *index_out = (size_t)(job - sh.jobs);
    }

    return job;
}

static bool continue_job(job_t *job, size_t index, const char *cmd) {
    if (!job || job->state != JOB_STOPPED) {
        return true;
    }

    if (kill(-job->pid, SIGCONT) < 0) {
        sh_printf("%s: failed to continue job\n", cmd);

        if (index < sh.job_count) {
            job_remove_index(index);
        }

        return false;
    }

    job->state = JOB_RUNNING;
    return true;
}

static int fg(int argc, char *const argv[]) {
    size_t index = 0;
    job_t *job = select_job(argc, argv, "fg", &index);
    if (!job) {
        return 1;
    }

    if (!continue_job(job, index, "fg")) {
        return 1;
    }

    tty_set_pgrp(job->pid);
    sh_wait_result_t wait_result = wait_foreground_pgrp(job->pid, 0);
    tty_set_pgrp(sh.pgid);

    if (wait_result.stopped) {
        job->state = JOB_STOPPED;
    } else if (index < sh.job_count) {
        job_remove_index(index);
    }

    return wait_result.stopped ? (128 + SIGTSTP) : wait_result.exit_status;
}

static int bg(int argc, char *const argv[]) {
    size_t index = 0;
    job_t *job = select_job(argc, argv, "bg", &index);
    if (!job) {
        return 1;
    }

    if (!continue_job(job, index, "bg")) {
        return 1;
    }

    return 0;
}

static int env_find(const char *key) {
    if (!key || !key[0]) {
        return -1;
    }

    for (size_t i = 0; i < sh.env_count; i++) {
        if (!strcmp(sh.env[i].key, key)) {
            return (int)i;
        }
    }

    return -1;
}

static const char *env_get(const char *key) {
    int index = env_find(key);
    if (index < 0) {
        return "";
    }

    return sh.env[index].value;
}

static bool env_set(const char *key, const char *value) {
    if (!key || !key[0] || !value) {
        return false;
    }

    int index = env_find(key);
    sh_env_t *entry = NULL;

    if (index >= 0) {
        entry = &sh.env[index];
    } else {
        if (sh.env_count >= SH_ENV_MAX) {
            return false;
        }

        entry = &sh.env[sh.env_count++];
        snprintf(entry->key, sizeof(entry->key), "%s", key);
    }

    snprintf(entry->value, sizeof(entry->value), "%s", value);
    if (!strcmp(key, "PATH")) {
        complete_set_path(entry->value);
    }

    if (!strcmp(key, "NO_COLOR")) {
        sh.no_color = entry->value[0] != '\0';
        complete_set_color_enabled(!sh.no_color);
    }

    return true;
}

static void env_unset(const char *key) {
    int index = env_find(key);
    if (index < 0) {
        return;
    }

    for (size_t i = (size_t)index; i + 1 < sh.env_count; i++) {
        sh.env[i] = sh.env[i + 1];
    }

    memset(&sh.env[sh.env_count - 1], 0, sizeof(sh.env[sh.env_count - 1]));
    sh.env_count--;

    if (!strcmp(key, "PATH")) {
        complete_set_path(NULL);
    }

    if (!strcmp(key, "NO_COLOR")) {
        sh.no_color = false;
        complete_set_color_enabled(true);
    }
}

static void env_print(void) {
    for (size_t i = 0; i < sh.env_count; i++) {
        sh_printf("%s=%s\n", sh.env[i].key, sh.env[i].value);
    }
}

static void update_pwd(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        env_set("PWD", cwd);
    }
}

static const char *prompt_path(void) {
    const char *pwd = env_get("PWD");
    const char *home = env_get("HOME");

    if (!pwd || !pwd[0]) {
        return "/";
    }

    static char short_pwd[PATH_MAX];

    if (home && home[0] && !strcmp(pwd, home)) {
        return "~";
    }

    size_t home_len = home ? strlen(home) : 0;
    if (home_len > 1 && !strncmp(pwd, home, home_len) && pwd[home_len] == '/') {
        snprintf(short_pwd, sizeof(short_pwd), "~%s", pwd + home_len);
        return short_pwd;
    }

    return pwd;
}

static void build_prompt(char *out, size_t out_len) {
    if (!out || !out_len) {
        return;
    }

    const char *path = prompt_path();
    const char *mark = getuid() == 0 ? "#" : "$";
    const char *mark_color = sh.last_status ? SH_C_RED : SH_C_GREEN;

    snprintf(
        out,
        out_len,
        "%s%s%s %s%s%s ",
        sh_color(SH_C_CYAN),
        path,
        sh_color(SH_C_RESET),
        sh_color(mark_color),
        mark,
        sh_color(SH_C_RESET)
    );
}

static bool env_key_is_valid(const char *key, size_t len) {
    if (!key || !len) {
        return false;
    }

    if (!(isalpha((unsigned char)key[0]) || key[0] == '_')) {
        return false;
    }

    for (size_t i = 1; i < len; i++) {
        char ch = key[i];
        if (!(isalnum((unsigned char)ch) || ch == '_')) {
            return false;
        }
    }

    return true;
}

static bool apply_assignment_token(const char *token) {
    if (!token || !token[0]) {
        return false;
    }

    const char *eq = strchr(token, '=');
    if (!eq) {
        return false;
    }

    size_t key_len = (size_t)(eq - token);
    if (!env_key_is_valid(token, key_len)) {
        return false;
    }

    char key[SH_ENV_KEY_MAX];
    size_t copy_len = key_len;
    if (copy_len >= sizeof(key)) {
        copy_len = sizeof(key) - 1;
    }

    memcpy(key, token, copy_len);
    key[copy_len] = '\0';

    return env_set(key, eq + 1);
}

static void expand_arg(const char *in, char *out, size_t out_len) {
    if (!in || !out || !out_len) {
        return;
    }

    size_t o = 0;
    size_t i = 0;

    if (in[0] == '~' && (in[1] == '\0' || in[1] == '/')) {
        const char *home = env_get("HOME");

        if (home && home[0]) {
            size_t home_len = strlen(home);

            if (o + home_len >= out_len) {
                home_len = out_len - o - 1;
            }

            memcpy(out + o, home, home_len);

            o += home_len;
            i = 1;
        }
    }

    for (; in[i] && o + 1 < out_len; i++) {
        if (in[i] != '$') {
            out[o++] = in[i];
            continue;
        }

        size_t start = i + 1;
        size_t end = start;
        char key[SH_ENV_KEY_MAX] = { 0 };

        if (in[start] == '{') {
            start++;
            end = start;

            while (in[end] && in[end] != '}') {
                end++;
            }
        } else if (in[start] == '?' || in[start] == '!' || in[start] == '$') {
            end = start + 1;
        } else {
            while (isalnum((unsigned char)in[end]) || in[end] == '_') {
                end++;
            }
        }

        if (end == start) {
            out[o++] = '$';
            continue;
        }

        size_t key_len = end - start;
        if (key_len >= sizeof(key)) {
            key_len = sizeof(key) - 1;
        }

        memcpy(key, in + start, key_len);
        key[key_len] = '\0';

        char special[32] = { 0 };
        const char *value = NULL;

        if (!strcmp(key, "?")) {
            snprintf(special, sizeof(special), "%d", sh.last_status);
            value = special;
        } else if (!strcmp(key, "!")) {
            if (sh.last_bg_pid > 0) {
                snprintf(special, sizeof(special), "%ld", (long)sh.last_bg_pid);
                value = special;
            } else {
                value = "";
            }
        } else if (!strcmp(key, "$")) {
            snprintf(special, sizeof(special), "%ld", (long)sh.pgid);
            value = special;
        } else {
            value = env_get(key);
        }

        size_t value_len = strlen(value);

        if (o + value_len >= out_len) {
            value_len = out_len - o - 1;
        }

        memcpy(out + o, value, value_len);
        o += value_len;

        if (in[i + 1] == '{' && in[end] == '}') {
            i = end;
        } else {
            i = end - 1;
        }
    }

    out[o] = '\0';
}

typedef struct {
    bool quote_open;
    bool trailing_escape;
} sh_cont_state_t;

static sh_cont_state_t continuation_state(const char *line) {
    sh_cont_state_t state = { 0 };

    if (!line) {
        return state;
    }

    bool in_single = false;
    bool in_double = false;
    bool escape = false;

    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
        len--;
    }

    for (size_t i = 0; i < len; i++) {
        char ch = line[i];

        if (escape) {
            escape = false;
            continue;
        }

        if (!in_single && ch == '\\') {
            escape = true;
            continue;
        }

        if (!in_double && ch == '\'') {
            in_single = !in_single;
            continue;
        }

        if (!in_single && ch == '"') {
            in_double = !in_double;
            continue;
        }
    }

    state.quote_open = in_single || in_double;
    state.trailing_escape = escape;
    return state;
}

static int read_line_fd(int fd, char *buf, size_t len, bool interactive) {
    if (!buf || !len) {
        return -1;
    }

    size_t pos = 0;
    bool cr_seen = false;

    while (pos + 1 < len) {
        if (interactive && sh.got_sigint) {
            sh.got_sigint = 0;
            return -1;
        }

        char ch = 0;
        ssize_t read_count = read(fd, &ch, 1);

        if (!read_count) {
            break;
        }

        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (ch == '\r') {
            ch = '\n';
            cr_seen = true;
        } else if (ch == '\n' && cr_seen) {
            cr_seen = false;
            continue;
        } else {
            cr_seen = false;
        }

        buf[pos++] = ch;

        if (ch == '\n') {
            break;
        }
    }

    if (!pos && !interactive) {
        return -1;
    }

    buf[pos] = '\0';
    return 0;
}

static bool is_operator(const char *token) {
    if (!token || !token[0]) {
        return false;
    }

    if (!strcmp(token, "|") || !strcmp(token, "&")) {
        return true;
    }

    if (!strcmp(token, "<") || !strcmp(token, ">") || !strcmp(token, ">>")) {
        return true;
    }

    return false;
}

static char *redir_target_or_error(char **tokens, int token_count, int *index_io, const char *error_text) {
    if (!tokens || !index_io || !error_text) {
        return NULL;
    }

    int index = *index_io;
    if (index + 1 >= token_count || is_operator(tokens[index + 1])) {
        io_write_str(error_text);
        return NULL;
    }

    *index_io = index + 1;
    return tokens[*index_io];
}

static int tokenize(const char *line, char *storage, size_t storage_len, char **tokens, int max_tokens) {
    if (!line || !storage || !storage_len || !tokens || max_tokens <= 1) {
        return 0;
    }

    int count = 0;
    const char *src = line;
    char *dst = storage;
    char *end = storage + storage_len - 1;

    while (*src && count < max_tokens - 1) {
        while (*src && isspace((unsigned char)*src)) {
            src++;
        }

        if (!*src) {
            break;
        }

        if (*src == '|' || *src == '<' || *src == '>' || *src == '&') {
            if (dst >= end) {
                break;
            }

            tokens[count++] = dst;

            if (*src == '>' && src[1] == '>') {
                if (dst + 2 > end) {
                    break;
                }

                *dst++ = '>';
                *dst++ = '>';
                src += 2;
            } else {
                *dst++ = *src++;
            }

            *dst++ = '\0';
            continue;
        }

        if (dst >= end) {
            break;
        }

        tokens[count++] = dst;

        bool in_single = false;
        bool in_double = false;

        while (*src) {
            char ch = *src;

            if (!in_single && ch == '"') {
                in_double = !in_double;
                src++;
                continue;
            }

            if (!in_double && ch == '\'') {
                in_single = !in_single;
                src++;
                continue;
            }

            if (ch == '\\' && !in_single) {
                src++;
                if (!*src) {
                    break;
                }
                if (dst >= end) {
                    break;
                }

                *dst++ = *src++;
                continue;
            }

            if (!in_single && !in_double) {
                if (isspace((unsigned char)ch)) {
                    src++;
                    break;
                }

                if (ch == '|' || ch == '<' || ch == '>' || ch == '&') {
                    break;
                }
            }

            if (dst >= end) {
                break;
            }

            *dst++ = ch;
            src++;
        }

        if (dst >= end) {
            break;
        }

        *dst++ = '\0';
    }

    *dst = '\0';
    tokens[count] = NULL;
    return count;
}

static int parse_pipeline(
    const char *line,
    sh_stage_t *stages,
    int *stage_count_out,
    bool *background_out,
    char *token_store,
    size_t token_store_len,
    char **tokens,
    int token_cap
) {
    if (!line || !stages || !stage_count_out || !background_out || !token_store || !token_store_len || !tokens ||
        token_cap <= 1) {
        return -1;
    }

    int token_count = tokenize(line, token_store, token_store_len, tokens, token_cap);

    if (token_count <= 0) {
        return 0;
    }

    memset(stages, 0, sizeof(sh_stage_t) * SH_MAX_STAGES);

    int stage = 0;
    bool background = false;

    for (int i = 0; i < token_count; i++) {
        const char *token = tokens[i];

        if (!strcmp(token, "&")) {
            if (i != token_count - 1) {
                io_write_str("sh: syntax error near '&'\n");
                return -1;
            }

            background = true;
            continue;
        }

        if (!strcmp(token, "|")) {
            if (!stages[stage].argc || stage + 1 >= SH_MAX_STAGES) {
                io_write_str("sh: invalid pipeline\n");
                return -1;
            }
            stage++;
            continue;
        }

        if (!strcmp(token, "<")) {
            stages[stage].in_path = redir_target_or_error(tokens, token_count, &i, "sh: invalid input redirection\n");
            if (!stages[stage].in_path) {
                return -1;
            }
            continue;
        }

        if (!strcmp(token, ">") || !strcmp(token, ">>")) {
            stages[stage].out_path = redir_target_or_error(tokens, token_count, &i, "sh: invalid output redirection\n");
            if (!stages[stage].out_path) {
                return -1;
            }

            stages[stage].out_append = !strcmp(token, ">>");
            continue;
        }

        if (stages[stage].argc >= SH_MAX_ARGS - 1) {
            io_write_str("sh: too many arguments\n");
            return -1;
        }

        stages[stage].argv[stages[stage].argc++] = tokens[i];
    }

    if (!stages[stage].argc) {
        io_write_str("sh: empty command\n");
        return -1;
    }

    for (int i = 0; i <= stage; i++) {
        stages[i].argv[stages[i].argc] = NULL;
    }

    *stage_count_out = stage + 1;
    *background_out = background;
    return 1;
}

static void env_build_exec(char env_data[SH_ENV_MAX][SH_ENV_ENTRY_MAX], char *envp[SH_ENV_MAX + 1]) {
    size_t count = sh.env_count;
    if (count > SH_ENV_MAX) {
        count = SH_ENV_MAX;
    }

    for (size_t i = 0; i < count; i++) {
        snprintf(env_data[i], SH_ENV_ENTRY_MAX, "%s=%s", sh.env[i].key, sh.env[i].value);
        envp[i] = env_data[i];
    }

    envp[count] = NULL;
}

static void exec_script(const char *script, char *const argv[], char *const envp[]) {
    char *sh_args[SH_MAX_ARGS];
    int argc = 0;

    sh_args[argc++] = "sh";
    sh_args[argc++] = (char *)script;

    if (argv) {
        for (int i = 1; argv[i] && argc < SH_MAX_ARGS - 1; i++) {
            sh_args[argc++] = argv[i];
        }
    }

    sh_args[argc] = NULL;
    execve("/bin/sh", sh_args, envp);
}

static bool file_has_elf_magic(const char *path) {
    if (!path || !path[0]) {
        return false;
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }

    unsigned char magic[4] = { 0 };
    ssize_t n = read(fd, magic, sizeof(magic));
    close(fd);

    if (n != (ssize_t)sizeof(magic)) {
        return false;
    }

    return magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

static bool build_exec_path(char *out, size_t out_len, const char *dir, size_t dir_len, const char *cmd) {
    if (!out || !dir || !cmd || !out_len) {
        errno = EINVAL;
        return false;
    }

    int written = 0;
    if (dir_len > 0 && dir[dir_len - 1] != '/') {
        written = snprintf(out, out_len, "%.*s/%s", (int)dir_len, dir, cmd);
    } else {
        written = snprintf(out, out_len, "%.*s%s", (int)dir_len, dir, cmd);
    }

    if (written < 0 || (size_t)written >= out_len) {
        errno = ENAMETOOLONG;
        return false;
    }

    return true;
}

static bool exec_in_path(const char *cmd, char *const argv[], char *const envp[]) {
    if (!cmd || !cmd[0]) {
        return false;
    }

    if (strlen(cmd) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }

    if (strchr(cmd, '/')) {
        execve(cmd, argv, envp);

        if (errno == ENOEXEC && !file_has_elf_magic(cmd)) {
            exec_script(cmd, argv, envp);
        }

        return false;
    }

    const char *path = env_get("PATH");
    if (!path || !path[0]) {
        path = "/bin";
    }
    const char *cursor = path;
    char full[PATH_MAX];
    int last_error = ENOENT;

    while (*cursor) {
        const char *next = strchr(cursor, ':');
        size_t len = next ? (size_t)(next - cursor) : strlen(cursor);

        if (!build_exec_path(full, sizeof(full), cursor, len, cmd)) {
            if (errno != ENOENT) {
                last_error = errno;
            }
        } else {
            execve(full, argv, envp);

            if (errno == ENOEXEC && !file_has_elf_magic(full)) {
                exec_script(full, argv, envp);
                return false;
            }

            if (errno != ENOENT && errno != ENOTDIR) {
                last_error = errno;
            }
        }

        if (!next) {
            break;
        }

        cursor = next + 1;
    }

    errno = last_error;
    return false;
}

static void print_exec_error(const char *cmd, int err) {
    const char *name = (cmd && cmd[0]) ? cmd : "<null>";

    switch (err) {
    case ENOENT:
        sh_errorf("sh: %s: command not found\n", name);
        return;
    case EACCES:
        sh_errorf("sh: %s: permission denied (not executable)\n", name);
        return;
    case ENOEXEC:
        sh_errorf("sh: %s: unsupported executable format\n", name);
        return;
    case EISDIR:
        sh_errorf("sh: %s: is a directory\n", name);
        return;
    case ENOTDIR:
        sh_errorf("sh: %s: not a directory\n", name);
        return;
    case ENAMETOOLONG:
        sh_errorf("sh: %s: command name too long\n", name);
        return;
    default:
        sh_errorf("sh: %s: %s\n", name, strerror(err));
        return;
    }
}

static bool parse_umask(const char *text, mode_t *out) {
    if (!text || !text[0] || !out) {
        return false;
    }

    if (text[0] == '-' || text[0] == '+') {
        return false;
    }

    char *end = NULL;
    long value = strtol(text, &end, 8);
    if (!end || *end || value < 0 || value > 0777) {
        return false;
    }

    *out = (mode_t)value;
    return true;
}

static const char *sh_builtin_names[] = {
    "help",
    "echo",
    "exit",
    "set",
    "export",
    "unset",
    "env",
    "cd",
    "umask",
    "history",
    "jobs",
    "fg",
    "bg",
    "time",
    "where",
    "type",
    NULL,
};

static bool is_builtin_name(const char *name) {
    if (!name || !name[0]) {
        return false;
    }

    for (const char **it = sh_builtin_names; *it; it++) {
        if (!strcmp(*it, name)) {
            return true;
        }
    }

    return false;
}

static int builtin_where(int argc, char *const argv[]) {
    const char *cmd = (argc > 0 && argv[0]) ? argv[0] : "where";

    if (argc < 2) {
        sh_errorf("%s: usage: %s NAME...\n", cmd, cmd);
        return 1;
    }

    const char *path = env_get("PATH");
    if (!path || !path[0]) {
        path = "/bin";
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        if (!name || !name[0]) {
            continue;
        }

        bool found = false;

        if (is_builtin_name(name)) {
            sh_printf("%s%s%s: shell built-in\n", sh_color(SH_C_CYAN), name, sh_color(SH_C_RESET));
            found = true;
        }

        if (strchr(name, '/')) {
            if (access(name, X_OK) == 0) {
                sh_printf("%s\n", name);
                found = true;
            }
        } else {
            const char *cursor = path;
            char full[PATH_MAX];

            while (*cursor) {
                const char *next = strchr(cursor, ':');
                size_t len = next ? (size_t)(next - cursor) : strlen(cursor);

                if (build_exec_path(full, sizeof(full), cursor, len, name) && access(full, X_OK) == 0) {
                    sh_printf("%s%s%s\n", sh_color(SH_C_GREEN), full, sh_color(SH_C_RESET));
                    found = true;
                }

                if (!next) {
                    break;
                }

                cursor = next + 1;
            }
        }

        if (!found) {
            sh_errorf("%s: %s: not found\n", cmd, name);
            status = 1;
        }
    }

    return status;
}

static void set_status(int *status, int value) {
    if (status) {
        *status = value;
    }
}

static void builtin_help(void) {
    sh_printf(
        "%sBuiltins%s\n"
        "  %-10s %s\n"
        "  %-10s %s\n"
        "  %-10s %s\n"
        "  %-10s %s\n"
        "  %-10s %s\n"
        "  %-10s %s\n"
        "  %-10s %s\n"
        "  %-10s %s\n"
        "  %-10s %s\n"
        "  %-10s %s\n"
        "  %-10s %s\n",
        sh_color(SH_C_CYAN),
        sh_color(SH_C_RESET),
        "cd [dir]",
        "change directory; cd - returns to the previous directory",
        "set",
        "print variables or set NAME=VALUE",
        "export",
        "same as set, for familiar scripts",
        "unset",
        "remove a shell variable",
        "where/type",
        "show whether a name is a builtin or executable",
        "history",
        "show command history",
        "jobs fg bg",
        "manage stopped/background jobs",
        "time",
        "measure a command's elapsed wall time",
        "; && ||",
        "chain commands by sequence or status",
        "echo env",
        "print text or environment",
        "help exit",
        "show this help or leave the shell"
    );
}

static void builtin_set(int argc, char *const argv[], int *status) {
    if (argc == 1) {
        env_print();
        return;
    }

    char *eq = strchr(argv[1], '=');
    if (eq) {
        *eq = '\0';
        if (!env_set(argv[1], eq + 1)) {
            set_status(status, 1);
        }

        return;
    }

    if (argc >= 3) {
        if (!env_set(argv[1], argv[2])) {
            set_status(status, 1);
        }

        return;
    }

    io_write_str("set: usage: set NAME=VALUE\n");
    set_status(status, 1);
}

static void builtin_unset(int argc, char *const argv[], int *status) {
    if (argc < 2) {
        io_write_str("unset: usage: unset NAME\n");
        set_status(status, 1);
        return;
    }

    env_unset(argv[1]);
}

static void builtin_echo(int argc, char *const argv[]) {
    for (int i = 1; i < argc; i++) {
        char expanded[SH_EXPAND_MAX] = { 0 };

        expand_arg(argv[i], expanded, sizeof(expanded));
        io_write_str(expanded);

        if (i + 1 < argc) {
            io_write_str(" ");
        }
    }

    io_write_str("\n");
}

static int parse_exit_status(const char *text, int *out) {
    if (!text || !text[0] || !out) {
        return -1;
    }

    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (!end || *end) {
        return -1;
    }

    *out = (int)(value & 0xff);
    return 0;
}

static void builtin_exit(int argc, char *const argv[], int *status) {
    int code = sh.last_status;

    if (argc > 2) {
        sh_errorf("exit: too many arguments\n");
        set_status(status, 1);
        return;
    }

    if (argc == 2 && parse_exit_status(argv[1], &code) < 0) {
        sh_errorf("exit: %s: numeric argument required\n", argv[1]);
        _exit(2);
    }

    _exit(code);
}

static void builtin_cd(int argc, char *const argv[], int *status) {
    const char *target = "/";

    if (argc >= 2 && argv[1] && argv[1][0]) {
        target = argv[1];
    }

    const char *old_pwd = env_get("PWD");
    if (!strcmp(target, "-")) {
        target = env_get("OLDPWD");

        if (!target || !target[0]) {
            sh_errorf("cd: OLDPWD not set\n");
            set_status(status, 1);
            return;
        }
    }

    if (chdir(target) < 0) {
        sh_errorf("cd: %s: %s\n", target, strerror(errno));
        set_status(status, 1);
        return;
    }

    if (old_pwd && old_pwd[0]) {
        env_set("OLDPWD", old_pwd);
    }

    update_pwd();

    if (argc >= 2 && argv[1] && !strcmp(argv[1], "-")) {
        sh_printf("%s\n", env_get("PWD"));
    }
}

static void builtin_umask(int argc, char *const argv[], int *status) {
    if (argc == 1) {
        mode_t old = umask(0);
        umask(old);
        sh_printf("%03o\n", (unsigned int)(old & 0777));
        return;
    }

    if (argc == 2) {
        mode_t mask = 0;

        if (!parse_umask(argv[1], &mask)) {
            io_write_str("umask: usage: umask [ooo]\n");
            set_status(status, 1);
            return;
        }

        umask(mask);
        return;
    }

    io_write_str("umask: usage: umask [ooo]\n");
    set_status(status, 1);
}

static bool handle_builtin(int argc, char *const argv[], int *status) {
    set_status(status, 0);

    if (argc <= 0) {
        return false;
    }

    if (!strcmp(argv[0], "exit")) {
        builtin_exit(argc, argv, status);
        return true;
    }

    if (!strcmp(argv[0], "help")) {
        builtin_help();
        return true;
    }

    if (!strcmp(argv[0], "env")) {
        env_print();
        return true;
    }

    if (!strcmp(argv[0], "set")) {
        builtin_set(argc, argv, status);
        return true;
    }

    if (!strcmp(argv[0], "export")) {
        builtin_set(argc, argv, status);
        return true;
    }

    if (!strcmp(argv[0], "unset")) {
        builtin_unset(argc, argv, status);
        return true;
    }

    if (!strcmp(argv[0], "echo")) {
        builtin_echo(argc, argv);
        return true;
    }

    if (!strcmp(argv[0], "cd")) {
        builtin_cd(argc, argv, status);
        return true;
    }

    if (!strcmp(argv[0], "umask")) {
        builtin_umask(argc, argv, status);
        return true;
    }

    if (!strcmp(argv[0], "jobs")) {
        print_jobs();
        return true;
    }

    if (!strcmp(argv[0], "time")) {
        sh_errorf("time: usage: time COMMAND [ARG...]\n");
        set_status(status, 1);
        return true;
    }

    if (!strcmp(argv[0], "history")) {
        history_print();
        return true;
    }

    if (!strcmp(argv[0], "fg")) {
        set_status(status, fg(argc, argv));
        return true;
    }

    if (!strcmp(argv[0], "bg")) {
        set_status(status, bg(argc, argv));
        return true;
    }

    if (!strcmp(argv[0], "where") || !strcmp(argv[0], "type")) {
        set_status(status, builtin_where(argc, argv));
        return true;
    }

    return false;
}

static void close_pipe_fds(int pipes[][2], int count) {
    for (int i = 0; i < count; i++) {
        if (pipes[i][0] >= 0) {
            close(pipes[i][0]);
            pipes[i][0] = -1;
        }

        if (pipes[i][1] >= 0) {
            close(pipes[i][1]);
            pipes[i][1] = -1;
        }
    }
}

static void close_start_gate(int gate[2]) {
    if (gate[0] >= 0) {
        close(gate[0]);
        gate[0] = -1;
    }

    if (gate[1] >= 0) {
        close(gate[1]);
        gate[1] = -1;
    }
}

static void wait_start_gate(int gate[2]) {
    if (gate[0] < 0) {
        return;
    }

    if (gate[1] >= 0) {
        close(gate[1]);
        gate[1] = -1;
    }

    char byte = 0;
    while (read(gate[0], &byte, 1) < 0 && errno == EINTR) {
        ;
    }

    close(gate[0]);
    gate[0] = -1;
}

static void release_start_gate(int gate[2]) {
    if (gate[1] < 0) {
        return;
    }

    if (gate[0] >= 0) {
        close(gate[0]);
        gate[0] = -1;
    }

    char byte = 0;
    while (write(gate[1], &byte, 1) < 0 && errno == EINTR) {
        ;
    }

    close(gate[1]);
    gate[1] = -1;
}

static int redirect_path_to_fd(const char *path, int open_flags, mode_t mode, int target_fd, const char *label) {
    int fd = open(path, open_flags, mode);
    if (fd < 0) {
        sh_errorf("sh: %s: %s: %s\n", label, path ? path : "", strerror(errno));
        return -1;
    }

    if (dup2(fd, target_fd) < 0) {
        close(fd);
        io_write_str("sh: dup failed\n");
        return -1;
    }

    close(fd);
    return 0;
}

static int open_redirection(const sh_stage_t *stage) {
    if (!stage) {
        return 0;
    }

    if (stage->in_path && stage->in_path[0]) {
        int input_rc = redirect_path_to_fd(stage->in_path, O_RDONLY, 0, STDIN_FILENO, "input");
        if (input_rc < 0) {
            return -1;
        }
    }

    if (stage->out_path && stage->out_path[0]) {
        int flags = O_WRONLY | O_CREAT;

        if (stage->out_append) {
            flags |= O_APPEND;
        } else {
            flags |= O_TRUNC;
        }

        int redir_rc = redirect_path_to_fd(stage->out_path, flags, 0644, STDOUT_FILENO, "output");

        if (redir_rc < 0) {
            return -1;
        }
    }

    return 0;
}

static int restore_redirected_fd(int saved_fd, int target_fd) {
    if (saved_fd < 0) {
        return 0;
    }

    int rc = 0;
    if (dup2(saved_fd, target_fd) < 0) {
        io_write_str("sh: failed to restore redirected fd\n");
        rc = -1;
    }

    close(saved_fd);
    return rc;
}

static int run_builtin_in_shell(const sh_stage_t *stage, int *status_out) {
    if (!stage) {
        return -1;
    }

    int saved_stdin = -1;
    int saved_stdout = -1;

    if (stage->in_path && stage->in_path[0]) {
        saved_stdin = dup(STDIN_FILENO);
        if (saved_stdin < 0) {
            io_write_str("sh: failed to save stdin\n");
            return -1;
        }
    }

    if (stage->out_path && stage->out_path[0]) {
        saved_stdout = dup(STDOUT_FILENO);
        if (saved_stdout < 0) {
            if (saved_stdin >= 0) {
                close(saved_stdin);
            }
            io_write_str("sh: failed to save stdout\n");
            return -1;
        }
    }

    if (open_redirection(stage) < 0) {
        (void)restore_redirected_fd(saved_stdin, STDIN_FILENO);
        (void)restore_redirected_fd(saved_stdout, STDOUT_FILENO);
        return -1;
    }

    int builtin_status = 0;
    bool handled = handle_builtin(stage->argc, stage->argv, &builtin_status);

    int restore_rc = 0;
    if (restore_redirected_fd(saved_stdin, STDIN_FILENO) < 0) {
        restore_rc = -1;
    }
    if (restore_redirected_fd(saved_stdout, STDOUT_FILENO) < 0) {
        restore_rc = -1;
    }

    if (!handled || restore_rc < 0) {
        return -1;
    }

    if (status_out) {
        *status_out = builtin_status;
    }

    return 0;
}

static int run_pipeline(sh_stage_t *stages, int stage_count, bool background, const char *cmdline) {
    int pipes[SH_MAX_STAGES - 1][2];

    for (int i = 0; i < SH_MAX_STAGES - 1; i++) {
        pipes[i][0] = -1;
        pipes[i][1] = -1;
    }

    int start_gate[2] = { -1, -1 };
    bool gate_child_start = sh.interactive && !background;
    if (gate_child_start && pipe(start_gate) < 0) {
        io_write_str("sh: pipe failed\n");
        return -1;
    }

    for (int i = 0; i + 1 < stage_count; i++) {
        if (pipe(pipes[i]) < 0) {
            io_write_str("sh: pipe failed\n");
            close_start_gate(start_gate);
            close_pipe_fds(pipes, stage_count - 1);
            return -1;
        }
    }

    pid_t pgid = 0;
    pid_t last_pid = 0;
    for (int i = 0; i < stage_count; i++) {
        pid_t pid = fork();

        if (!pid) {
            pid_t target_pgid = (!pgid) ? getpid() : pgid;

            setpgid(0, target_pgid);

            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGWINCH, SIG_DFL);

            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0) {
                    _exit(1);
                }
            }

            if (i + 1 < stage_count) {
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0) {
                    _exit(1);
                }
            }

            close_pipe_fds(pipes, stage_count - 1);

            if (open_redirection(&stages[i]) < 0) {
                _exit(1);
            }

            if (gate_child_start) {
                wait_start_gate(start_gate);
            }

            int builtin_status = 0;
            if (handle_builtin(stages[i].argc, stages[i].argv, &builtin_status)) {
                _exit(builtin_status);
            }

            char env_data[SH_ENV_MAX][SH_ENV_ENTRY_MAX];
            char *envp[SH_ENV_MAX + 1];
            env_build_exec(env_data, envp);

            exec_in_path(stages[i].argv[0], stages[i].argv, envp);
            print_exec_error(stages[i].argv[0], errno);

            _exit(1);
        }

        if (pid < 0) {
            io_write_str("sh: fork failed\n");
            close_start_gate(start_gate);
            close_pipe_fds(pipes, stage_count - 1);
            return -1;
        }

        if (!pgid) {
            pgid = pid;
        }

        last_pid = pid;
        setpgid(pid, pgid);
    }

    close_pipe_fds(pipes, stage_count - 1);

    if (background) {
        close_start_gate(start_gate);
        sh.last_bg_pid = pgid;
        if (sh.interactive) {
            job_t *job = job_add(pgid, cmdline, JOB_RUNNING);

            if (job) {
                sh_printf("[%d] %d\n", job->id, (int)pgid);
            }
        }

        return 0;
    }

    if (sh.interactive) {
        tty_set_pgrp(pgid);
    }

    release_start_gate(start_gate);

    sh_wait_result_t wait_result = wait_foreground_pgrp(pgid, last_pid);
    if (sh.interactive) {
        tty_set_pgrp(sh.pgid);
    }

    if (wait_result.stopped) {
        job_add(pgid, cmdline, JOB_STOPPED);
        return 128 + SIGTSTP;
    }

    return wait_result.exit_status;
}

typedef struct {
    char expanded[SH_MAX_STAGES][SH_MAX_ARGS][SH_EXPAND_MAX];
    char in_paths[SH_MAX_STAGES][SH_EXPAND_MAX];
    char out_paths[SH_MAX_STAGES][SH_EXPAND_MAX];
} sh_expand_t;

static void expand_stages(
    sh_stage_t *stages,
    int stage_count,
    char expanded[SH_MAX_STAGES][SH_MAX_ARGS][SH_EXPAND_MAX],
    char in_paths[SH_MAX_STAGES][SH_EXPAND_MAX],
    char out_paths[SH_MAX_STAGES][SH_EXPAND_MAX]
) {
    for (int i = 0; i < stage_count; i++) {
        for (int a = 0; a < stages[i].argc; a++) {
            expand_arg(stages[i].argv[a], expanded[i][a], sizeof(expanded[i][a]));
            stages[i].argv[a] = expanded[i][a];
        }

        stages[i].argv[stages[i].argc] = NULL;

        if (stages[i].in_path && stages[i].in_path[0]) {
            expand_arg(stages[i].in_path, in_paths[i], sizeof(in_paths[i]));
            stages[i].in_path = in_paths[i];
        }

        if (stages[i].out_path && stages[i].out_path[0]) {
            expand_arg(stages[i].out_path, out_paths[i], sizeof(out_paths[i]));
            stages[i].out_path = out_paths[i];
        }
    }
}

static bool strip_time_prefix(sh_stage_t *stages, int stage_count) {
    if (!stages || stage_count <= 0 || stages[0].argc <= 0) {
        return false;
    }

    if (strcmp(stages[0].argv[0], "time")) {
        return false;
    }

    for (int i = 1; i < stages[0].argc; i++) {
        stages[0].argv[i - 1] = stages[0].argv[i];
    }

    stages[0].argc--;
    stages[0].argv[stages[0].argc] = NULL;

    return true;
}

static char *trim_ascii_whitespace(char *text) {
    if (!text) {
        return NULL;
    }

    while (*text && isspace((unsigned char)*text)) {
        text++;
    }

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[len - 1] = '\0';
        len--;
    }

    return text;
}

static const char *chain_op_text(sh_chain_op_t op) {
    switch (op) {
    case SH_CHAIN_AND:
        return "&&";
    case SH_CHAIN_OR:
        return "||";
    case SH_CHAIN_ALWAYS:
    default:
        return ";";
    }
}

static bool add_clause(sh_clause_t *clauses, int *count, int max_clauses, sh_chain_op_t op, char *text) {
    if (!clauses || !count || !text) {
        return false;
    }

    char *trimmed = trim_ascii_whitespace(text);
    if (!trimmed || !trimmed[0]) {
        sh_errorf("sh: syntax error near '%s'\n", chain_op_text(op));
        return false;
    }

    if (*count >= max_clauses) {
        sh_errorf("sh: too many command clauses\n");
        return false;
    }

    clauses[*count].op = op;
    clauses[*count].text = trimmed;
    (*count)++;

    return true;
}

static int split_command_list(char *line, sh_clause_t *clauses, int max_clauses) {
    if (!line || !clauses || max_clauses <= 0) {
        return -1;
    }

    bool in_single = false;
    bool in_double = false;
    bool escape = false;
    int count = 0;
    char *start = line;
    sh_chain_op_t next_op = SH_CHAIN_ALWAYS;

    for (char *cursor = line; *cursor; cursor++) {
        char ch = *cursor;

        if (escape) {
            escape = false;
            continue;
        }

        if (!in_single && ch == '\\') {
            escape = true;
            continue;
        }

        if (!in_double && ch == '\'') {
            in_single = !in_single;
            continue;
        }

        if (!in_single && ch == '"') {
            in_double = !in_double;
            continue;
        }

        if (in_single || in_double) {
            continue;
        }

        sh_chain_op_t op = SH_CHAIN_ALWAYS;
        size_t op_len = 0;

        if (ch == ';') {
            op = SH_CHAIN_ALWAYS;
            op_len = 1;
        } else if (ch == '&' && cursor[1] == '&') {
            op = SH_CHAIN_AND;
            op_len = 2;
        } else if (ch == '|' && cursor[1] == '|') {
            op = SH_CHAIN_OR;
            op_len = 2;
        } else {
            continue;
        }

        *cursor = '\0';

        char *trimmed = trim_ascii_whitespace(start);
        if (!trimmed || !trimmed[0]) {
            if (op == SH_CHAIN_ALWAYS && count > 0) {
                start = cursor + op_len;
                next_op = op;
                cursor += op_len - 1;
                continue;
            }

            sh_errorf("sh: syntax error near '%s'\n", chain_op_text(op));
            return -1;
        }

        if (!add_clause(clauses, &count, max_clauses, next_op, trimmed)) {
            return -1;
        }

        start = cursor + op_len;
        next_op = op;
        cursor += op_len - 1;
    }

    char *tail = trim_ascii_whitespace(start);
    if (!tail || !tail[0]) {
        if (next_op == SH_CHAIN_ALWAYS) {
            return count;
        }

        sh_errorf("sh: syntax error near '%s'\n", chain_op_text(next_op));
        return -1;
    }

    if (!add_clause(clauses, &count, max_clauses, next_op, tail)) {
        return -1;
    }

    return count;
}

static int run_single_command(char *line) {
    if (!line || !line[0]) {
        return 0;
    }

    size_t line_len = strlen(line);
    if (line_len && line[line_len - 1] == '\n') {
        line[line_len - 1] = '\0';
    }

    if (!line[0]) {
        return 0;
    }

    char cmdline[SH_CMD_MAX];
    snprintf(cmdline, sizeof(cmdline), "%s", line);

    sh_stage_t stages[SH_MAX_STAGES];
    char token_store[SH_LINE_MAX];
    char *tokens[SH_MAX_TOKENS];
    int stage_count = 0;
    bool background = false;

    int parse_ret = parse_pipeline(
        line,
        stages,
        &stage_count,
        &background,
        token_store,
        sizeof(token_store),
        tokens,
        SH_MAX_TOKENS
    );

    if (parse_ret < 0) {
        return 1;
    }

    if (!parse_ret) {
        return 0;
    }

    sh_expand_t *exp = malloc(sizeof(sh_expand_t));
    if (!exp) {
        io_write_str("sh: out of memory\n");
        return -1;
    }

    expand_stages(stages, stage_count, exp->expanded, exp->in_paths, exp->out_paths);

    bool timed = strip_time_prefix(stages, stage_count);
    if (timed && stages[0].argc == 0) {
        sh_errorf("time: usage: time COMMAND [ARG...]\n");
        free(exp);
        return 1;
    }

    sh_time_t time_start = { 0 };
    bool have_time = timed && sh_time_begin(&time_start);

    bool simple_builtin = stage_count == 1 && !background;

    if (simple_builtin && stages[0].argc == 1) {
        if (apply_assignment_token(stages[0].argv[0])) {
            if (have_time) {
                sh_print_time(&time_start);
            }

            free(exp);
            return 0;
        }
    }

    if (simple_builtin && is_builtin_name(stages[0].argv[0])) {
        int builtin_status = 0;
        if (run_builtin_in_shell(&stages[0], &builtin_status) == 0) {
            if (have_time) {
                sh_print_time(&time_start);
            }

            free(exp);
            return builtin_status;
        }

        free(exp);
        return 1;
    }

    int ret = run_pipeline(stages, stage_count, background, cmdline);

    if (have_time) {
        sh_print_time(&time_start);
    }

    free(exp);

    if (ret < 0) {
        return 1;
    }

    return ret;
}

static int run_command(char *line) {
    if (!line || !line[0]) {
        return 0;
    }

    size_t line_len = strlen(line);
    if (line_len && line[line_len - 1] == '\n') {
        line[line_len - 1] = '\0';
    }

    char *trimmed = trim_ascii_whitespace(line);
    if (!trimmed || !trimmed[0]) {
        return 0;
    }

    sh_clause_t clauses[SH_MAX_CLAUSES];
    int clause_count = split_command_list(trimmed, clauses, SH_MAX_CLAUSES);
    if (clause_count < 0) {
        sh.last_status = 1;
        return 1;
    }

    int status = 0;
    for (int i = 0; i < clause_count; i++) {
        if (clauses[i].op == SH_CHAIN_AND && status != 0) {
            continue;
        }

        if (clauses[i].op == SH_CHAIN_OR && status == 0) {
            continue;
        }

        status = run_single_command(clauses[i].text);
        sh.last_status = status;
    }

    return status;
}

static int run_script(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    int fd = open(path, O_RDONLY, 0);

    if (fd < 0) {
        io_write_str("sh: failed to open script\n");
        return -1;
    }

    char line[SH_LINE_MAX];

    while (!read_line_fd(fd, line, sizeof(line), false)) {
        char *cursor = line;

        while (*cursor && isspace((unsigned char)*cursor)) {
            cursor++;
        }

        if (!*cursor || *cursor == '#') {
            continue;
        }

        run_command(cursor);
    }

    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    char line[SH_LINE_MAX];
    sh.interactive = argc <= 1;
    sh.color = sh.interactive;

    sh.pgid = getpid();
    if (sh.interactive) {
        setpgid(0, 0);
        tty_set_pgrp(sh.pgid);
    }

    signal(SIGINT, sigint_handler);
    signal(SIGWINCH, sigwinch_handler);
    signal(SIGCHLD, sigchld_handler);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    input_set_sigint_flag(&sh.got_sigint);
    input_set_sigwinch_flag(&sh.got_sigwinch);
    input_set_sigchld_flag(&sh.got_sigchld);
    input_set_sigchld_callback(reap_jobs_signal);

    env_set("PATH", "/bin");
    env_set("HOME", "/");
    env_set("SHELL", "/bin/sh");
    env_set("TERM", "apheleia");

    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0]) {
        env_set("NO_COLOR", no_color);
    }

    struct passwd *pwd = getpwuid(getuid());
    if (pwd && pwd->pw_dir && pwd->pw_dir[0]) {
        env_set("HOME", pwd->pw_dir);
    }

    env_set("PWD", "/");
    update_pwd();

    if (argc > 2 && !strcmp(argv[1], "-c")) {
        char cmdline[SH_LINE_MAX];
        snprintf(cmdline, sizeof(cmdline), "%s", argv[2]);
        return run_command(cmdline);
    }

    if (argc > 1) {
        return run_script(argv[1]);
    }

    tty_set_pgrp(sh.pgid);

    for (;;) {
        char prompt[PATH_MAX + 32];

        reap_jobs(true);
        build_prompt(prompt, sizeof(prompt));

        if (read_line_interactive(prompt, line, sizeof(line), true) < 0) {
            io_write_str("\n");
            continue;
        }

        while (1) {
            sh_cont_state_t cont = continuation_state(line);

            if (!cont.quote_open && !cont.trailing_escape) {
                break;
            }

            size_t len = strlen(line);

            if (cont.trailing_escape) {
                if (len > 0 && line[len - 1] == '\n') {
                    line[--len] = '\0';
                }
                if (len > 0 && line[len - 1] == '\\') {
                    line[--len] = '\0';
                }
            }

            if (len + 2 >= sizeof(line)) {
                io_write_str("sh: line too long\n");
                break;
            }

            int read_rc = read_line_interactive("> ", line + len, sizeof(line) - len, false);

            if (read_rc < 0) {
                io_write_str("\n");
                line[0] = '\0';
                break;
            }
        }

        if (!line[0]) {
            continue;
        }

        history_add(line);
        run_command(line);
    }

    return 0;
}
