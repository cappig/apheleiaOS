#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

static ssize_t write_str(const char* str) {
    if (!str)
        return 0;

    return write(STDOUT_FILENO, str, strlen(str));
}

static void print_prompt(void) {
    write_str("sh$ ");
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
    char* cursor = line;

    while (*cursor && argc < max - 1) {
        while (*cursor && isspace((unsigned char)*cursor))
            cursor++;

        if (!*cursor)
            break;

        argv[argc++] = cursor;

        while (*cursor && !isspace((unsigned char)*cursor))
            cursor++;

        if (*cursor)
            *cursor++ = '\0';
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

static bool sh_exec_in_path(const char* cmd, char* const argv[]) {
    if (!cmd || !cmd[0])
        return false;

    if (strchr(cmd, '/')) {
        execve(cmd, argv, NULL);
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
        write_str("builtins: help, echo, exit, set, unset, env, cd\n");
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

    return 0;
}

static int run_command(char* line, char** args, char expanded[][SH_EXPAND_MAX], char** exec_args) {
    strip_newline(line);

    if (!line[0])
        return 0;

    int cmd_argc = split_line(line, args, SH_MAX_ARGS);
    if (!cmd_argc)
        return 0;

    if (handle_builtin(cmd_argc, args))
        return 0;

    for (int i = 0; i < cmd_argc; i++) {
        sh_expand_arg(args[i], expanded[i], sizeof(expanded[i]));
        exec_args[i] = expanded[i];
    }
    exec_args[cmd_argc] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        sh_exec_in_path(exec_args[0], exec_args);
        write_str("sh: exec failed\n");
        _exit(1);
    }

    if (pid < 0) {
        write_str("sh: fork failed\n");
        return -1;
    }

    tty_set_pgrp(pid);
    int status = 0;
    wait(pid, &status);
    tty_set_pgrp(getpid());
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

    signal(SIGINT, sigint_handler);
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
    tty_set_pgrp(getpid());

    for (;;) {
        print_prompt();

        if (read_line_fd(STDIN_FILENO, line, sizeof(line), true) < 0) {
            write_str("\n");
            continue;
        }

        run_command(line, args, expanded, exec_args);
    }

    return 0;
}
