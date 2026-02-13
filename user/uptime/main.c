#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static bool read_uptime(unsigned long long* sec_out) {
    if (!sec_out)
        return false;

    char buf[64] = {0};
    if (sysctl("kern.uptime", buf, sizeof(buf)) < 0)
        return false;

    unsigned long long sec = 0;
    if (sscanf(buf, "%llu", &sec) != 1)
        return false;

    *sec_out = sec;
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
    if (days > 0)
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
    else
        snprintf(out, sizeof(out), "up %02llu:%02llu:%02llu\n", hrs, mins, secs);

    write(STDOUT_FILENO, out, strlen(out));
    return 0;
}
