#include <kv.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool read_uptime(unsigned long long *sec_out) {
    if (!sec_out) {
        return false;
    }

    char buf[256] = {0};
    if (kv_read_file("/dev/clock", buf, sizeof(buf)) <= 0) {
        return false;
    }

    unsigned long long now = 0;
    unsigned long long boot = 0;

    if (!kv_read_u64(buf, "now", &now)) {
        return false;
    }

    if (!kv_read_u64(buf, "boot", &boot)) {
        return false;
    }

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
    } else {
        snprintf(out, sizeof(out), "up %02llu:%02llu:%02llu\n", hrs, mins, secs);
    }

    write(STDOUT_FILENO, out, strlen(out));
    return 0;
}
