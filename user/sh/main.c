#include <string.h>
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
    }

    return 0;
}
