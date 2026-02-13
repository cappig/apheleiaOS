#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef ARCH_NAME
#define ARCH_NAME "unknown"
#endif

static void print_row(const char* left, const char* right) {
    char line[256];
    snprintf(line, sizeof(line), "%-12s %s\n", left ? left : "", right ? right : "");
    write(STDOUT_FILENO, line, strlen(line));
}

static bool read_sysctl_string(const char* name, char* out, size_t out_len) {
    if (!name || !out || out_len < 2)
        return false;

    ssize_t ret = sysctl(name, out, out_len);
    if (ret < 0) {
        out[0] = '\0';
        return false;
    }

    out[out_len - 1] = '\0';
    return true;
}

int main(void) {
    passwd_t pwd = {0};
    uid_t uid = getuid();
    bool have_pwd = getpwuid(uid, &pwd) == 0;

    char user[64];
    char shell[128];

    if (have_pwd && pwd.pw_name[0])
        snprintf(user, sizeof(user), "%s", pwd.pw_name);
    else
        snprintf(user, sizeof(user), "%llu", (unsigned long long)uid);

    if (have_pwd && pwd.pw_shell[0])
        snprintf(shell, sizeof(shell), "%s", pwd.pw_shell);
    else
        snprintf(shell, sizeof(shell), "/sbin/sh");

    const char* owl[] = {"    ,___,   ", "    (o,o)   ", "    /)_)    ", "     \" \"    "};

    char user_at[96];
    char os_line[96];
    char shell_line[160];
    char ram_line[128];
    char cpu_line[128];
    char sep[96];

    char os_name[32] = "apheleiaOS";
    char os_arch[32] = ARCH_NAME;
    char cpu_model[32] = "<unknown>";
    char value[64] = {0};
    unsigned long long total_kib = 0;
    unsigned long long free_kib = 0;
    unsigned long long freq_khz = 0;

    if (read_sysctl_string("kern.ostype", value, sizeof(value)))
        snprintf(os_name, sizeof(os_name), "%s", value);
    if (read_sysctl_string("kern.arch", value, sizeof(value)))
        snprintf(os_arch, sizeof(os_arch), "%s", value);
    if (read_sysctl_string("vm.mem.total_kib", value, sizeof(value)))
        total_kib = (unsigned long long)atoll(value);
    if (read_sysctl_string("vm.mem.free_kib", value, sizeof(value)))
        free_kib = (unsigned long long)atoll(value);
    if (read_sysctl_string("hw.model", value, sizeof(value)))
        snprintf(cpu_model, sizeof(cpu_model), "%s", value);
    if (read_sysctl_string("hw.clockrate", value, sizeof(value)))
        freq_khz = (unsigned long long)atoll(value);

    snprintf(user_at, sizeof(user_at), "%s@$sysname", user);
    snprintf(os_line, sizeof(os_line), "os: %s %s", os_name, os_arch);
    snprintf(shell_line, sizeof(shell_line), "shell: %s", shell);

    if (total_kib > 0) {
        unsigned long long used_kib = total_kib >= free_kib ? (total_kib - free_kib) : 0;
        snprintf(
            ram_line, sizeof(ram_line), "ram: %llu/%llu MiB", used_kib / 1024, total_kib / 1024
        );
    } else {
        snprintf(ram_line, sizeof(ram_line), "ram: <unknown>");
    }

    if (freq_khz > 0)
        snprintf(cpu_line, sizeof(cpu_line), "cpu: %s @ %llu MHz", cpu_model, freq_khz / 1000);
    else
        snprintf(cpu_line, sizeof(cpu_line), "cpu: %s", cpu_model);

    size_t sep_len = strlen(user_at);
    if (sep_len >= sizeof(sep))
        sep_len = sizeof(sep) - 1;

    for (size_t i = 0; i < sep_len; i++)
        sep[i] = '-';

    sep[sep_len] = '\0';

    const char* right_rows[] = {
        user_at,
        sep,
        os_line,
        shell_line,
        ram_line,
        cpu_line,
    };

    size_t owl_rows = sizeof(owl) / sizeof(owl[0]);
    size_t right_rows_count = sizeof(right_rows) / sizeof(right_rows[0]);

    size_t owl_top_pad = 0;
    if (right_rows_count > owl_rows)
        owl_top_pad = (right_rows_count - owl_rows) / 2;

    for (size_t i = 0; i < right_rows_count; i++) {
        const char* left = "";
        if (i >= owl_top_pad) {
            size_t owl_idx = i - owl_top_pad;
            if (owl_idx < owl_rows)
                left = owl[owl_idx];
        }

        print_row(left, right_rows[i]);
    }

    return 0;
}
