#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/proc.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "complete.h"
#include "input.h"

static volatile sig_atomic_t got_sigint = 0;
static volatile sig_atomic_t got_sigwinch = 0;

#define SH_ENV_MAX       32
#define SH_ENV_KEY_MAX   32
#define SH_ENV_VAL_MAX   128
#define SH_EXPAND_MAX    256
#define SH_MAX_ARGS      16
#define SH_MAX_TOKENS    64
#define SH_MAX_STAGES    8
#define SH_MAX_JOBS      16
#define SH_CMD_MAX       128
#define SH_LINE_MAX      SH_INPUT_LINE_MAX
#define SH_ENV_ENTRY_MAX (SH_ENV_KEY_MAX + SH_ENV_VAL_MAX + 2)

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

static sh_env_t sh_env[SH_ENV_MAX];
static size_t sh_env_count = 0;
static job_t sh_jobs[SH_MAX_JOBS];
static size_t sh_job_count = 0;
static int sh_next_job_id = 1;
static pid_t sh_pgid = 0;

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

static void sigint_handler(int signum) {
    (void)signum;
    got_sigint = 1;
}

static void sigwinch_handler(int signum) {
    (void)signum;
    got_sigwinch = 1;
}

static void tty_set_pgrp(pid_t pid) {
    if (pid <= 0) {
        return;
    }

    ioctl(STDIN_FILENO, TIOCSPGRP, &pid);
}

static job_t *job_find_by_id(int id) {
    for (size_t i = 0; i < sh_job_count; i++) {
        if (sh_jobs[i].id == id) {
            return &sh_jobs[i];
        }
    }

    return NULL;
}

static void job_remove_index(size_t index) {
    if (index >= sh_job_count) {
        return;
    }

    for (size_t i = index; i + 1 < sh_job_count; i++) {
        sh_jobs[i] = sh_jobs[i + 1];
    }

    memset(&sh_jobs[sh_job_count - 1], 0, sizeof(sh_jobs[sh_job_count - 1]));
    sh_job_count--;
}

static job_t *job_add(pid_t pid, const char *cmd, job_state_t state) {
    if (sh_job_count >= SH_MAX_JOBS) {
        return NULL;
    }

    job_t *job = &sh_jobs[sh_job_count++];
    memset(job, 0, sizeof(*job));
    job->id = sh_next_job_id++;
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

        proc_stat_t stat = {0};
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
    for (size_t i = 0; i < sh_job_count;) {
        job_t *job = &sh_jobs[i];
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

static void print_jobs(void) {
    reap_jobs(false);

    for (size_t i = 0; i < sh_job_count; i++) {
        job_t *job = &sh_jobs[i];
        const char *state = job->state == JOB_STOPPED ? "Stopped" : "Running";
        sh_printf("[%d] %s  %s\n", job->id, state, job->cmd);
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

static bool wait_foreground_pgrp(pid_t pgid) {
    if (pgid <= 0) {
        return false;
    }

    for (;;) {
        int status = 0;
        pid_t waited = waitpid(-pgid, &status, WUNTRACED);

        if (waited > 0) {
            if (WIFSTOPPED(status)) {
                return true;
            }

            continue;
        }

        if (!waited) {
            continue;
        }

        if (errno == EINTR) {
            continue;
        }

        return false;
    }
}

static job_t *select_job(int argc, char **argv, const char *cmd, size_t *index_out) {
    if (!sh_job_count) {
        sh_printf("%s: no jobs\n", cmd);
        return NULL;
    }

    job_t *job = NULL;
    if (argc < 2) {
        job = &sh_jobs[sh_job_count - 1];
    } else {
        job = job_find_by_id(parse_job_id(argv[1]));
    }

    if (!job) {
        sh_printf("%s: no such job\n", cmd);
        return NULL;
    }

    if (index_out) {
        *index_out = (size_t)(job - sh_jobs);
    }

    return job;
}

static bool continue_job(job_t *job, size_t index, const char *cmd) {
    if (!job || job->state != JOB_STOPPED) {
        return true;
    }

    if (kill(-job->pid, SIGCONT) < 0) {
        sh_printf("%s: failed to continue job\n", cmd);

        if (index < sh_job_count) {
            job_remove_index(index);
        }

        return false;
    }

    job->state = JOB_RUNNING;
    return true;
}

static int fg(int argc, char **argv) {
    size_t index = 0;
    job_t *job = select_job(argc, argv, "fg", &index);
    if (!job) {
        return 1;
    }

    if (!continue_job(job, index, "fg")) {
        return 1;
    }

    tty_set_pgrp(job->pid);
    bool stopped = wait_foreground_pgrp(job->pid);
    tty_set_pgrp(sh_pgid);

    if (stopped) {
        job->state = JOB_STOPPED;
    } else if (index < sh_job_count) {
        job_remove_index(index);
    }

    return 1;
}

static int bg(int argc, char **argv) {
    size_t index = 0;
    job_t *job = select_job(argc, argv, "bg", &index);
    if (!job) {
        return 1;
    }

    continue_job(job, index, "bg");

    return 1;
}

static int env_find(const char *key) {
    if (!key || !key[0]) {
        return -1;
    }

    for (size_t i = 0; i < sh_env_count; i++) {
        if (!strcmp(sh_env[i].key, key)) {
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

    return sh_env[index].value;
}

static bool env_set(const char *key, const char *value) {
    if (!key || !key[0] || !value) {
        return false;
    }

    int index = env_find(key);
    sh_env_t *entry = NULL;

    if (index >= 0) {
        entry = &sh_env[index];
    } else {
        if (sh_env_count >= SH_ENV_MAX) {
            return false;
        }

        entry = &sh_env[sh_env_count++];
        snprintf(entry->key, sizeof(entry->key), "%s", key);
    }

    snprintf(entry->value, sizeof(entry->value), "%s", value);
    if (!strcmp(key, "PATH")) {
        complete_set_path(entry->value);
    }

    return true;
}

static void env_unset(const char *key) {
    int index = env_find(key);
    if (index < 0) {
        return;
    }

    for (size_t i = (size_t)index; i + 1 < sh_env_count; i++) {
        sh_env[i] = sh_env[i + 1];
    }

    memset(&sh_env[sh_env_count - 1], 0, sizeof(sh_env[sh_env_count - 1]));
    sh_env_count--;

    if (!strcmp(key, "PATH")) {
        complete_set_path(NULL);
    }
}

static void env_print(void) {
    for (size_t i = 0; i < sh_env_count; i++) {
        sh_printf("%s=%s\n", sh_env[i].key, sh_env[i].value);
    }
}

static void update_pwd(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        env_set("PWD", cwd);
    }
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
        char key[SH_ENV_KEY_MAX] = {0};

        if (in[start] == '{') {
            start++;
            end = start;
            while (in[end] && in[end] != '}') {
                end++;
            }
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

        const char *value = env_get(key);
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
    sh_cont_state_t state = {0};

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
        if (interactive && got_sigint) {
            got_sigint = 0;
            return -1;
        }

        char ch = 0;
        ssize_t read_count = read(fd, &ch, 1);

        if (!read_count) {
            break;
        }

        if (read_count < 0) {
            continue;
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

    return !strcmp(token, "|") || !strcmp(token, "<") || !strcmp(token, ">") ||
           !strcmp(token, ">>") || !strcmp(token, "&");
}

static char *
redir_target_or_error(char **tokens, int token_count, int *index_io, const char *error_text) {
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

static int
tokenize(const char *line, char *storage, size_t storage_len, char **tokens, int max_tokens) {
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

static int
parse_pipeline(char *line, sh_stage_t *stages, int *stage_count_out, bool *background_out) {
    if (!line || !stages || !stage_count_out || !background_out) {
        return -1;
    }

    char token_store[SH_LINE_MAX];
    char *tokens[SH_MAX_TOKENS];

    int token_count = tokenize(line, token_store, sizeof(token_store), tokens, SH_MAX_TOKENS);
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
            stages[stage].in_path =
                redir_target_or_error(tokens, token_count, &i, "sh: invalid input redirection\n");
            if (!stages[stage].in_path) {
                return -1;
            }
            continue;
        }

        if (!strcmp(token, ">") || !strcmp(token, ">>")) {
            stages[stage].out_path =
                redir_target_or_error(tokens, token_count, &i, "sh: invalid output redirection\n");
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

static void
env_build_exec(char env_data[SH_ENV_MAX][SH_ENV_ENTRY_MAX], char *envp[SH_ENV_MAX + 1]) {
    size_t count = sh_env_count;
    if (count > SH_ENV_MAX) {
        count = SH_ENV_MAX;
    }

    for (size_t i = 0; i < count; i++) {
        snprintf(env_data[i], SH_ENV_ENTRY_MAX, "%s=%s", sh_env[i].key, sh_env[i].value);
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

static bool
build_exec_path(char *out, size_t out_len, const char *dir, size_t dir_len, const char *cmd) {
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

        if (errno == ENOEXEC) {
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

            if (errno == ENOEXEC) {
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
        sh_printf("sh: %s: command not found\n", name);
        return;
    case EACCES:
        sh_printf("sh: %s: permission denied (not executable)\n", name);
        return;
    case ENOEXEC:
        sh_printf("sh: %s: exec format error\n", name);
        return;
    case EISDIR:
        sh_printf("sh: %s: is a directory\n", name);
        return;
    case ENOTDIR:
        sh_printf("sh: %s: not a directory\n", name);
        return;
    case ENAMETOOLONG:
        sh_printf("sh: %s: command name too long\n", name);
        return;
    default:
        sh_printf("sh: %s: %s\n", name, strerror(err));
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

static int handle_builtin(int argc, char **argv) {
    if (argc <= 0) {
        return 0;
    }

    if (!strcmp(argv[0], "exit")) {
        _exit(0);
    }

    if (!strcmp(argv[0], "help")) {
        io_write_str(
            "builtins: help, echo, exit, set, unset, env, cd, umask, history, jobs, fg, bg\n"
        );
        return 1;
    }

    if (!strcmp(argv[0], "env")) {
        env_print();
        return 1;
    }

    if (!strcmp(argv[0], "set")) {
        if (argc == 1) {
            env_print();
            return 1;
        }

        char *eq = strchr(argv[1], '=');
        if (eq) {
            *eq = '\0';
            env_set(argv[1], eq + 1);
            return 1;
        }

        if (argc >= 3) {
            env_set(argv[1], argv[2]);
            return 1;
        }

        io_write_str("set: usage: set NAME=VALUE\n");

        return 1;
    }

    if (!strcmp(argv[0], "unset")) {
        if (argc < 2) {
            io_write_str("unset: usage: unset NAME\n");
            return 1;
        }

        env_unset(argv[1]);

        return 1;
    }

    if (!strcmp(argv[0], "echo")) {
        for (int i = 1; i < argc; i++) {
            char expanded[SH_EXPAND_MAX] = {0};

            expand_arg(argv[i], expanded, sizeof(expanded));
            io_write_str(expanded);

            if (i + 1 < argc) {
                io_write_str(" ");
            }
        }

        io_write_str("\n");
        return 1;
    }

    if (!strcmp(argv[0], "cd")) {
        const char *target = "/";

        if (argc >= 2 && argv[1] && argv[1][0]) {
            target = argv[1];
        }

        if (chdir(target) < 0) {
            io_write_str("cd: failed\n");
            return 1;
        }

        update_pwd();
        return 1;
    }

    if (!strcmp(argv[0], "umask")) {
        if (argc == 1) {
            mode_t old = umask(0);
            umask(old);
            sh_printf("%03o\n", (unsigned int)(old & 0777));
            return 1;
        }

        if (argc == 2) {
            mode_t mask = 0;

            if (!parse_umask(argv[1], &mask)) {
                io_write_str("umask: usage: umask [ooo]\n");
                return 1;
            }

            umask(mask);
            return 1;
        }

        io_write_str("umask: usage: umask [ooo]\n");
        return 1;
    }

    if (!strcmp(argv[0], "jobs")) {
        print_jobs();
        return 1;
    }

    if (!strcmp(argv[0], "history")) {
        history_print();
        return 1;
    }

    if (!strcmp(argv[0], "fg")) {
        return fg(argc, argv);
    }

    if (!strcmp(argv[0], "bg")) {
        return bg(argc, argv);
    }

    return 0;
}

static void close_pipe_fds(int pipes[][2], int count) {
    for (int i = 0; i < count; i++) {
        if (pipes[i][0] >= 0) {
            close(pipes[i][0]);
        }

        if (pipes[i][1] >= 0) {
            close(pipes[i][1]);
        }
    }
}

static int redirect_path_to_fd(
    const char *path,
    int open_flags,
    mode_t mode,
    int target_fd,
    const char *open_err
) {
    int fd = open(path, open_flags, mode);
    if (fd < 0) {
        io_write_str(open_err);
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

    if (stage->in_path && stage->in_path[0] &&
        redirect_path_to_fd(
            stage->in_path, O_RDONLY, 0, STDIN_FILENO, "sh: failed to open input\n"
        ) < 0) {
        return -1;
    }

    if (stage->out_path && stage->out_path[0]) {
        int flags = O_WRONLY | O_CREAT;

        if (stage->out_append) {
            flags |= O_APPEND;
        } else {
            flags |= O_TRUNC;
        }

        if (redirect_path_to_fd(
                stage->out_path, flags, 0644, STDOUT_FILENO, "sh: failed to open output\n"
            ) < 0) {
            return -1;
        }
    }

    return 0;
}

static int run_pipeline(sh_stage_t *stages, int stage_count, bool background, const char *cmdline) {
    int pipes[SH_MAX_STAGES - 1][2];

    for (int i = 0; i < SH_MAX_STAGES - 1; i++) {
        pipes[i][0] = -1;
        pipes[i][1] = -1;
    }

    for (int i = 0; i + 1 < stage_count; i++) {
        if (pipe(pipes[i]) < 0) {
            io_write_str("sh: pipe failed\n");
            close_pipe_fds(pipes, stage_count - 1);
            return -1;
        }
    }

    pid_t pgid = 0;
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

            if (handle_builtin(stages[i].argc, stages[i].argv)) {
                _exit(0);
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
            close_pipe_fds(pipes, stage_count - 1);
            return -1;
        }

        if (!pgid) {
            pgid = pid;
        }

        setpgid(pid, pgid);
    }

    close_pipe_fds(pipes, stage_count - 1);

    if (background) {
        job_t *job = job_add(pgid, cmdline, JOB_RUNNING);

        if (job) {
            sh_printf("[%d] %d\n", job->id, (int)pgid);
        }

        return 0;
    }

    tty_set_pgrp(pgid);

    bool stopped = wait_foreground_pgrp(pgid);

    tty_set_pgrp(sh_pgid);

    if (stopped) {
        job_add(pgid, cmdline, JOB_STOPPED);
    }

    return 0;
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

static int run_command(char *line) {
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
    int stage_count = 0;
    bool background = false;

    int parse_ret = parse_pipeline(line, stages, &stage_count, &background);

    if (parse_ret <= 0) {
        return 0;
    }

    sh_expand_t *exp = malloc(sizeof(sh_expand_t));
    if (!exp) {
        io_write_str("sh: out of memory\n");
        return -1;
    }

    expand_stages(stages, stage_count, exp->expanded, exp->in_paths, exp->out_paths);

    bool simple_builtin =
        stage_count == 1 && !background && !stages[0].in_path && !stages[0].out_path;

    if (simple_builtin && handle_builtin(stages[0].argc, stages[0].argv)) {
        free(exp);
        return 0;
    }

    int ret = run_pipeline(stages, stage_count, background, cmdline);
    free(exp);
    return ret;
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

    sh_pgid = getpid();
    setpgid(0, 0);
    tty_set_pgrp(sh_pgid);

    signal(SIGINT, sigint_handler);
    signal(SIGWINCH, sigwinch_handler);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    input_set_sigint_flag(&got_sigint);
    input_set_sigwinch_flag(&got_sigwinch);

    env_set("PATH", "/bin");
    env_set("HOME", "/");

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

    io_write_str("apheleiaOS sh\n");
    tty_set_pgrp(sh_pgid);

    for (;;) {
        reap_jobs(true);

        if (read_line_interactive("sh$ ", line, sizeof(line), true) < 0) {
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

            if (read_line_interactive("> ", line + len, sizeof(line) - len, false) < 0) {
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
