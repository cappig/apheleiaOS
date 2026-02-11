#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/proc.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t got_sigint = 0;

#define SH_ENV_MAX     32
#define SH_ENV_KEY_MAX 32
#define SH_ENV_VAL_MAX 128
#define SH_EXPAND_MAX  256
#define SH_MAX_ARGS    16
#define SH_MAX_JOBS    16
#define SH_CMD_MAX     128

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

static sh_env_t sh_env[SH_ENV_MAX];
static size_t sh_env_count = 0;
static job_t sh_jobs[SH_MAX_JOBS];
static size_t sh_job_count = 0;
static int sh_next_job_id = 1;
static pid_t sh_pgid = 0;

static ssize_t write_str(const char* str) {
    if (!str)
        return 0;

    return write(STDOUT_FILENO, str, strlen(str));
}

static void print_prompt(void) {
    write_str("\rsh$ ");
}

static void sigint_handler(int signum) {
    (void)signum;
    got_sigint = 1;
}

static void tty_set_pgrp(pid_t pid) {
    if (pid <= 0)
        return;

    ioctl(STDIN_FILENO, TIOCSPGRP, &pid);
}

static job_t* sh_job_find_by_id(int id) {
    for (size_t i = 0; i < sh_job_count; i++) {
        if (sh_jobs[i].id == id)
            return &sh_jobs[i];
    }

    return NULL;
}

static job_t* sh_job_find_by_pid(pid_t pid) {
    for (size_t i = 0; i < sh_job_count; i++) {
        if (sh_jobs[i].pid == pid)
            return &sh_jobs[i];
    }

    return NULL;
}

static void sh_job_remove_index(size_t index) {
    if (index >= sh_job_count)
        return;

    for (size_t i = index; i + 1 < sh_job_count; i++)
        sh_jobs[i] = sh_jobs[i + 1];

    memset(&sh_jobs[sh_job_count - 1], 0, sizeof(sh_jobs[sh_job_count - 1]));
    sh_job_count--;
}

static job_t* sh_job_add(pid_t pid, const char* cmd, job_state_t state) {
    if (sh_job_count >= SH_MAX_JOBS)
        return NULL;

    job_t* job = &sh_jobs[sh_job_count++];
    memset(job, 0, sizeof(*job));
    job->id = sh_next_job_id++;
    job->pid = pid;
    job->state = state;
    snprintf(job->cmd, sizeof(job->cmd), "%s", cmd ? cmd : "");
    return job;
}

static ssize_t sh_get_procs(proc_info_t* out, size_t cap) {
    return getprocs(out, cap);
}

static const proc_info_t* sh_find_proc(pid_t pid, const proc_info_t* list, ssize_t count) {
    if (!list || count <= 0)
        return NULL;

    for (ssize_t i = 0; i < count; i++) {
        if (list[i].pid == pid)
            return &list[i];
    }

    return NULL;
}

static void sh_reap_jobs(bool report) {
    proc_info_t procs[128];
    ssize_t count = sh_get_procs(procs, sizeof(procs) / sizeof(procs[0]));

    for (size_t i = 0; i < sh_job_count; ) {
        job_t* job = &sh_jobs[i];
        const proc_info_t* info = (count > 0) ? sh_find_proc(job->pid, procs, count) : NULL;

        if (!info || info->state == PROC_STATE_ZOMBIE) {
            int status = 0;
            waitpid(job->pid, &status, 0);
            if (report) {
                char line[SH_CMD_MAX + 32];
                snprintf(line, sizeof(line), "[%d] Done    %s\n", job->id, job->cmd);
                write_str(line);
            }
            sh_job_remove_index(i);
            continue;
        }

        if (info->state == PROC_STATE_STOPPED)
            job->state = JOB_STOPPED;
        else
            job->state = JOB_RUNNING;

        i++;
    }
}

static void sh_print_jobs(void) {
    sh_reap_jobs(false);

    for (size_t i = 0; i < sh_job_count; i++) {
        job_t* job = &sh_jobs[i];
        const char* state = job->state == JOB_STOPPED ? "Stopped" : "Running";
        char line[SH_CMD_MAX + 32];
        snprintf(line, sizeof(line), "[%d] %s  %s\n", job->id, state, job->cmd);
        write_str(line);
    }
}

static int sh_parse_job_id(const char* arg) {
    if (!arg || !arg[0])
        return -1;

    if (arg[0] == '%')
        arg++;

    int id = 0;
    while (*arg) {
        if (!isdigit((unsigned char)*arg))
            return -1;
        id = id * 10 + (*arg - '0');
        arg++;
    }

    return id > 0 ? id : -1;
}

static int sh_fg(int argc, char** argv) {
    if (sh_job_count == 0) {
        write_str("fg: no jobs\n");
        return 1;
    }

    job_t* job = NULL;
    if (argc < 2) {
        job = &sh_jobs[sh_job_count - 1];
    } else {
        int id = sh_parse_job_id(argv[1]);
        job = sh_job_find_by_id(id);
    }

    if (!job) {
        write_str("fg: no such job\n");
        return 1;
    }

    if (job->state == JOB_STOPPED)
        kill(job->pid, SIGCONT);

    tty_set_pgrp(job->pid);

    int status = 0;
    pid_t waited = waitpid(job->pid, &status, WUNTRACED);

    tty_set_pgrp(sh_pgid);

    if (waited < 0) {
        write_str("fg: wait failed\n");
        return 1;
    }

    if (WIFSTOPPED(status)) {
        job->state = JOB_STOPPED;
        return 1;
    }

    job_t* found = sh_job_find_by_pid(job->pid);
    if (found) {
        size_t index = (size_t)(found - sh_jobs);
        sh_job_remove_index(index);
    }

    return 1;
}

static int sh_bg(int argc, char** argv) {
    if (sh_job_count == 0) {
        write_str("bg: no jobs\n");
        return 1;
    }

    job_t* job = NULL;
    if (argc < 2) {
        job = &sh_jobs[sh_job_count - 1];
    } else {
        int id = sh_parse_job_id(argv[1]);
        job = sh_job_find_by_id(id);
    }

    if (!job) {
        write_str("bg: no such job\n");
        return 1;
    }

    if (job->state == JOB_STOPPED) {
        kill(job->pid, SIGCONT);
        job->state = JOB_RUNNING;
    }

    return 1;
}

static int sh_env_find(const char* key) {
    if (!key || !key[0])
        return -1;

    for (size_t i = 0; i < sh_env_count; i++) {
        if (!strcmp(sh_env[i].key, key))
            return (int)i;
    }

    return -1;
}

static const char* sh_env_get(const char* key) {
    int index = sh_env_find(key);
    if (index < 0)
        return "";

    return sh_env[index].value;
}

static bool sh_env_set(const char* key, const char* value) {
    if (!key || !key[0] || !value)
        return false;

    int index = sh_env_find(key);
    if (index >= 0) {
        snprintf(sh_env[index].value, sizeof(sh_env[index].value), "%s", value);
        return true;
    }

    if (sh_env_count >= SH_ENV_MAX)
        return false;

    snprintf(sh_env[sh_env_count].key, sizeof(sh_env[sh_env_count].key), "%s", key);
    snprintf(sh_env[sh_env_count].value, sizeof(sh_env[sh_env_count].value), "%s", value);
    sh_env_count++;
    return true;
}

static void sh_env_unset(const char* key) {
    int index = sh_env_find(key);
    if (index < 0)
        return;

    for (size_t i = (size_t)index; i + 1 < sh_env_count; i++)
        sh_env[i] = sh_env[i + 1];

    memset(&sh_env[sh_env_count - 1], 0, sizeof(sh_env[sh_env_count - 1]));
    sh_env_count--;
}

static void sh_env_print(void) {
    for (size_t i = 0; i < sh_env_count; i++) {
        write_str(sh_env[i].key);
        write_str("=");
        write_str(sh_env[i].value);
        write_str("\n");
    }
}

static void sh_update_pwd(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        sh_env_set("PWD", cwd);
}

static void sh_expand_arg(const char* in, char* out, size_t out_len) {
    if (!in || !out || !out_len)
        return;

    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_len; i++) {
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
            while (in[end] && in[end] != '}')
                end++;
        } else {
            while (isalnum((unsigned char)in[end]) || in[end] == '_')
                end++;
        }

        if (end == start) {
            out[o++] = '$';
            continue;
        }

        size_t key_len = end - start;
        if (key_len >= sizeof(key))
            key_len = sizeof(key) - 1;

        memcpy(key, in + start, key_len);
        key[key_len] = '\0';

        const char* value = sh_env_get(key);
        size_t value_len = strlen(value);
        if (o + value_len >= out_len)
            value_len = out_len - o - 1;

        memcpy(out + o, value, value_len);
        o += value_len;

        if (in[i + 1] == '{' && in[end] == '}')
            i = end;
        else
            i = end - 1;
    }

    out[o] = '\0';
}

static void strip_newline(char* buf) {
    size_t len = strlen(buf);
    if (!len)
        return;

    if (buf[len - 1] == '\n')
        buf[len - 1] = '\0';
}

typedef struct {
    bool quote_open;
    bool trailing_escape;
} sh_cont_state_t;

static sh_cont_state_t sh_continuation_state(const char* line) {
    sh_cont_state_t state = {0};

    if (!line)
        return state;

    bool in_single = false;
    bool in_double = false;
    bool escape = false;

    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
        len--;

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

static int read_line_fd(int fd, char* buf, size_t len, bool interactive) {
    if (!buf || !len)
        return -1;

    size_t pos = 0;
    bool cr_seen = false;

    while (pos + 1 < len) {
        if (interactive && got_sigint) {
            got_sigint = 0;
            return -1;
        }

        char ch = 0;
        ssize_t read_count = read(fd, &ch, 1);

        if (read_count == 0)
            break;

        if (read_count < 0)
            continue;

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

        if (ch == '\n')
            break;
    }

    if (pos == 0 && !interactive)
        return -1;

    buf[pos] = '\0';
    return 0;
}

static int split_line(char* line, char** argv, int max) {
    if (!line || !argv || max <= 0)
        return 0;

    int argc = 0;
    char* src = line;
    char* dst = line;

    while (*src && argc < max - 1) {
        while (*src && isspace((unsigned char)*src))
            src++;

        if (!*src)
            break;

        argv[argc++] = dst;

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
                if (!*src)
                    break;
                *dst++ = *src++;
                continue;
            }

            if (!in_single && !in_double && isspace((unsigned char)ch)) {
                src++;
                break;
            }

            *dst++ = ch;
            src++;
        }

        *dst++ = '\0';
    }

    argv[argc] = NULL;
    return argc;
}

static const char* sh_env_path(void) {
    const char* value = sh_env_get("PATH");
    if (value && value[0])
        return value;

    return "/sbin";
}

static void sh_exec_script(const char* script, char* const argv[]) {
    char* sh_args[SH_MAX_ARGS];
    int argc = 0;

    sh_args[argc++] = "sh";
    sh_args[argc++] = (char*)script;

    if (argv) {
        for (int i = 1; argv[i] && argc < SH_MAX_ARGS - 1; i++)
            sh_args[argc++] = argv[i];
    }

    sh_args[argc] = NULL;
    execve("/sbin/sh", sh_args, NULL);
}

static bool sh_exec_in_path(const char* cmd, char* const argv[]) {
    if (!cmd || !cmd[0])
        return false;

    if (strchr(cmd, '/')) {
        execve(cmd, argv, NULL);
        if (errno == ENOEXEC)
            sh_exec_script(cmd, argv);
        return false;
    }

    const char* path = sh_env_path();
    const char* cursor = path;
    char full[128];

    while (*cursor) {
        const char* next = strchr(cursor, ':');
        size_t len = next ? (size_t)(next - cursor) : strlen(cursor);
        if (len > 0) {
            if (len >= sizeof(full))
                len = sizeof(full) - 1;
            memcpy(full, cursor, len);
            full[len] = '\0';

            if (full[len - 1] == '/')
                snprintf(full + len - 1, sizeof(full) - (len - 1), "/%s", cmd);
            else
                snprintf(full + len, sizeof(full) - len, "/%s", cmd);

            execve(full, argv, NULL);
            if (errno == ENOEXEC) {
                sh_exec_script(full, argv);
                return false;
            }
        }

        if (!next)
            break;
        cursor = next + 1;
    }

    return false;
}

static int handle_builtin(int argc, char** argv) {
    if (argc <= 0)
        return 0;

    if (!strcmp(argv[0], "exit")) {
        _exit(0);
    }

    if (!strcmp(argv[0], "help")) {
        write_str("builtins: help, echo, exit, set, unset, env, cd, jobs, fg, bg\n");
        return 1;
    }

    if (!strcmp(argv[0], "env")) {
        sh_env_print();
        return 1;
    }

    if (!strcmp(argv[0], "set")) {
        if (argc == 1) {
            sh_env_print();
            return 1;
        }

        char* eq = strchr(argv[1], '=');
        if (eq) {
            *eq = '\0';
            sh_env_set(argv[1], eq + 1);
            return 1;
        }

        if (argc >= 3) {
            sh_env_set(argv[1], argv[2]);
            return 1;
        }

        write_str("set: usage: set NAME=VALUE\n");
        return 1;
    }

    if (!strcmp(argv[0], "unset")) {
        if (argc < 2) {
            write_str("unset: usage: unset NAME\n");
            return 1;
        }

        sh_env_unset(argv[1]);
        return 1;
    }

    if (!strcmp(argv[0], "echo")) {
        for (int i = 1; i < argc; i++) {
            char expanded[SH_EXPAND_MAX] = {0};
            sh_expand_arg(argv[i], expanded, sizeof(expanded));
            write_str(expanded);
            if (i + 1 < argc)
                write_str(" ");
        }
        write_str("\n");
        return 1;
    }

    if (!strcmp(argv[0], "cd")) {
        const char* target = "/";
        if (argc >= 2 && argv[1] && argv[1][0])
            target = argv[1];

        if (chdir(target) < 0) {
            write_str("cd: failed\n");
            return 1;
        }

        sh_update_pwd();
        return 1;
    }

    if (!strcmp(argv[0], "jobs")) {
        sh_print_jobs();
        return 1;
    }

    if (!strcmp(argv[0], "fg"))
        return sh_fg(argc, argv);

    if (!strcmp(argv[0], "bg"))
        return sh_bg(argc, argv);

    return 0;
}

static int run_command(char* line, char** args, char expanded[][SH_EXPAND_MAX], char** exec_args) {
    strip_newline(line);

    if (!line[0])
        return 0;

    char cmdline[SH_CMD_MAX];
    snprintf(cmdline, sizeof(cmdline), "%s", line);

    int cmd_argc = split_line(line, args, SH_MAX_ARGS);
    if (!cmd_argc)
        return 0;

    bool background = false;
    if (cmd_argc > 0 && !strcmp(args[cmd_argc - 1], "&")) {
        background = true;
        args[cmd_argc - 1] = NULL;
        cmd_argc--;
        if (cmd_argc == 0)
            return 0;
    }

    if (handle_builtin(cmd_argc, args))
        return 0;

    for (int i = 0; i < cmd_argc; i++) {
        sh_expand_arg(args[i], expanded[i], sizeof(expanded[i]));
        exec_args[i] = expanded[i];
    }
    exec_args[cmd_argc] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        sh_exec_in_path(exec_args[0], exec_args);
        write_str("sh: exec failed\n");
        _exit(1);
    }

    if (pid < 0) {
        write_str("sh: fork failed\n");
        return -1;
    }

    setpgid(pid, pid);

    if (background) {
        job_t* job = sh_job_add(pid, cmdline, JOB_RUNNING);
        if (job) {
            char line_out[64];
            snprintf(line_out, sizeof(line_out), "[%d] %d\n", job->id, (int)pid);
            write_str(line_out);
        }
        return 0;
    }

    tty_set_pgrp(pid);
    int status = 0;
    pid_t waited = waitpid(pid, &status, WUNTRACED);
    tty_set_pgrp(sh_pgid);

    if (waited < 0)
        return -1;

    if (WIFSTOPPED(status)) {
        sh_job_add(pid, cmdline, JOB_STOPPED);
        return 0;
    }

    return 0;
}

static int run_script(const char* path) {
    if (!path || !path[0])
        return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        write_str("sh: failed to open script\n");
        return -1;
    }

    char line[256];
    char* args[SH_MAX_ARGS];
    char expanded[SH_MAX_ARGS][SH_EXPAND_MAX];
    char* exec_args[SH_MAX_ARGS];

    while (read_line_fd(fd, line, sizeof(line), false) == 0) {
        char* cursor = line;
        while (*cursor && isspace((unsigned char)*cursor))
            cursor++;

        if (!*cursor || *cursor == '#')
            continue;

        run_command(cursor, args, expanded, exec_args);
    }

    close(fd);
    return 0;
}

int main(int argc, char** argv) {
    char line[256];

    sh_pgid = getpid();
    setpgid(0, 0);
    tty_set_pgrp(sh_pgid);

    signal(SIGINT, sigint_handler);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    sh_env_set("PATH", "/sbin");
    sh_env_set("PWD", "/");
    sh_update_pwd();

    if (argc > 2 && !strcmp(argv[1], "-c")) {
        char cmdline[256];
        snprintf(cmdline, sizeof(cmdline), "%s", argv[2]);
        return run_command(cmdline, args, expanded, exec_args);
    }

    if (argc > 1)
        return run_script(argv[1]);

    write_str("apheleiaOS sh\n");
    tty_set_pgrp(sh_pgid);

    for (;;) {
        sh_reap_jobs(true);
        print_prompt();

        if (read_line_fd(STDIN_FILENO, line, sizeof(line), true) < 0) {
            write_str("\n");
            continue;
        }

        while (1) {
            sh_cont_state_t cont = sh_continuation_state(line);
            if (!cont.quote_open && !cont.trailing_escape)
                break;

            size_t len = strlen(line);

            if (cont.trailing_escape) {
                if (len > 0 && line[len - 1] == '\n')
                    line[--len] = '\0';
                if (len > 0 && line[len - 1] == '\\')
                    line[--len] = '\0';
            }

            if (len + 2 >= sizeof(line)) {
                write_str("sh: line too long\n");
                break;
            }

            write_str("> ");

            if (read_line_fd(STDIN_FILENO, line + len, sizeof(line) - len, true) < 0) {
                write_str("\n");
                line[0] = '\0';
                break;
            }
        }

        if (!line[0])
            continue;

        run_command(line, args, expanded, exec_args);
    }

    return 0;
}
