#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static bool read_kv_u64(const char* text, const char* key, unsigned long long* out) {
    if (!text || !key || !out)
        return false;

    const char* line = text;
    size_t key_len = strlen(key);

    for (;;) {
        const char* next = strchr(line, '\n');
        size_t line_len = next ? (size_t)(next - line) : strlen(line);

        if (line_len > key_len && !strncmp(line, key, key_len)) {
            line += key_len;
            break;
        }

        if (!next)
            return false;

        line = next + 1;
    }

    if (sscanf(line, "%llu", out) != 1)
        return false;

    return true;
}

static bool read_uptime(unsigned long long* sec_out) {
    if (!sec_out)
        return false;

    int fd = open("/dev/clock", O_RDONLY, 0);
    if (fd < 0)
        return false;

    char buf[256] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
        return false;

    buf[n] = '\0';

    unsigned long long now = 0;
    unsigned long long boot = 0;

    if (!read_kv_u64(buf, "now=", &now))
        return false;

    if (!read_kv_u64(buf, "boot=", &boot))
        return false;

    *sec_out = now >= boot ? now - boot : 0;
    return true;
}

int main(void) {
    unsigned long long sec = 0;

    if (!read_uptime(&sec)) {
        write(STDOUT_FILENO, "up <unknown>\n", 13);
        return 1;
    }

    unsigned long long days = sec / 86400;
    unsigned long long rem = sec % 86400;
    unsigned long long hrs = rem / 3600;

    rem %= 3600;

    unsigned long long mins = rem / 60;
    unsigned long long secs = rem % 60;

    char out[96];

    if (days > 0) {
        snprintf(
            out,
            sizeof(out),
            "up %llu day%s, %02llu:%02llu:%02llu\n",
            days,
            days == 1 ? "" : "s",
            hrs,
            mins,
            secs
        );
    }
    else {
        snprintf(out, sizeof(out), "up %02llu:%02llu:%02llu\n", hrs, mins, secs);
    }

    write(STDOUT_FILENO, out, strlen(out));
    return 0;
}
