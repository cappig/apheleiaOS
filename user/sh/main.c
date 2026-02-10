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

static int read_line(char* buf, size_t len) {
    if (!buf || !len)
        return -1;

    size_t pos = 0;
    bool cr_seen = false;

    while (pos + 1 < len) {
        char ch = 0;
        ssize_t read_count = read(STDIN_FILENO, &ch, 1);

        if (read_count <= 0)
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

    buf[pos] = '\0';
    return 0;
}

int main(void) {
    char line[256];

    write_str("apheleiaOS sh\n");
    tty_set_pgrp(getpid());

    for (;;) {
        print_prompt();

        if (read_line(line, sizeof(line)) < 0)
            continue;

        strip_newline(line);

        if (!line[0])
            continue;

        if (!strcmp(line, "exit")) {
            _exit(0);
        } else if (!strcmp(line, "help")) {
            write_str("builtins: help, echo, exit\n");
        } else if (!strncmp(line, "echo ", 5)) {
            write_str(line + 5);
            write_str("\n");
        } else {
            write_str("unknown command\n");
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
            continue;
        }

        tty_set_pgrp(pid);
        int status = 0;
        wait(pid, &status);
        tty_set_pgrp(getpid());
    }

    return 0;
}
