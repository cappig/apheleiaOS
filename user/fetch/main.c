#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef ARCH_NAME
#define ARCH_NAME "unknown"
#endif

static const char* owl[] = {
    "    ,___,   ",
    "    (o,o)   ",
    "    /)_)    ",
    "     \" \"    ",
};

static void print_row(const char* left, const char* right) {
    char line[256];
    snprintf(line, sizeof(line), "%-12s %s\n", left ? left : "", right ? right : "");
    write(STDOUT_FILENO, line, strlen(line));
}

static bool read_sysctl_string(const char* name, char* out, size_t out_len) {
    if (!name || !out || out_len < 2)
        return false;

    if (sysctl(name, out, out_len) < 0) {
        out[0] = '\0';
        return false;
    }

    out[out_len - 1] = '\0';
    return true;
}

static bool read_sysctl_u64(const char* name, unsigned long long* out) {
    if (!out)
        return false;

    char value[64] = {0};
    if (!read_sysctl_string(name, value, sizeof(value)))
        return false;

    *out = (unsigned long long)atoll(value);
    return true;
}

static void resolve_user(char* user, size_t user_len, char* shell, size_t shell_len) {
    if (!user || !user_len || !shell || !shell_len)
        return;

    uid_t uid = getuid();
    passwd_t pwd = {0};
    bool have_pwd = !getpwuid(uid, &pwd);

    if (have_pwd && pwd.pw_name[0])
        snprintf(user, user_len, "%s", pwd.pw_name);
    else
        snprintf(user, user_len, "%llu", (unsigned long long)uid);

    if (have_pwd && pwd.pw_shell[0])
        snprintf(shell, shell_len, "%s", pwd.pw_shell);
    else
        snprintf(shell, shell_len, "/sbin/sh");
}

static void fill_separator(char* out, size_t out_len, size_t width) {
    if (!out || !out_len)
        return;

    if (width >= out_len)
        width = out_len - 1;

    memset(out, '-', width);
    out[width] = '\0';
}

static void print_fetch_rows(
    const char* user_at,
    const char* sep,
    const char* os_line,
    const char* shell_line,
    const char* ram_line,
    const char* cpu_line
) {
    print_row("", user_at);
    print_row(owl[0], sep);
    print_row(owl[1], os_line);
    print_row(owl[2], shell_line);
    print_row(owl[3], ram_line);
    print_row("", cpu_line);
}

int main(void) {
    char user[64] = {0};
    char shell[128] = {0};
    resolve_user(user, sizeof(user), shell, sizeof(shell));

    char user_at[96];
    char os_line[96];
    char shell_line[160];
    char ram_line[128];
    char cpu_line[128];
    char sep[96];

    char os_name[32] = "apheleiaOS";
    char os_arch[32] = ARCH_NAME;
    char cpu_model[64] = "<unknown>";
    unsigned long long total_kib = 0;
    unsigned long long free_kib = 0;
    unsigned long long freq_khz = 0;

    read_sysctl_string("kern.ostype", os_name, sizeof(os_name));
    read_sysctl_string("kern.arch", os_arch, sizeof(os_arch));
    read_sysctl_string("hw.model", cpu_model, sizeof(cpu_model));
    read_sysctl_u64("vm.mem.total_kib", &total_kib);
    read_sysctl_u64("vm.mem.free_kib", &free_kib);
    read_sysctl_u64("hw.clockrate", &freq_khz);

    snprintf(user_at, sizeof(user_at), "%s@$sysname", user);
    snprintf(os_line, sizeof(os_line), "os: %s %s", os_name, os_arch);
    snprintf(shell_line, sizeof(shell_line), "shell: %s", shell);

    if (total_kib > 0) {
        unsigned long long used_kib = total_kib >= free_kib ? (total_kib - free_kib) : 0;
        snprintf(ram_line, sizeof(ram_line), "ram: %llu/%llu MiB", used_kib / 1024, total_kib / 1024);
    } else {
        snprintf(ram_line, sizeof(ram_line), "ram: <unknown>");
    }

    if (freq_khz > 0)
        snprintf(cpu_line, sizeof(cpu_line), "cpu: %s @ %llu MHz", cpu_model, freq_khz / 1000);
    else
        snprintf(cpu_line, sizeof(cpu_line), "cpu: %s", cpu_model);

    fill_separator(sep, sizeof(sep), strlen(user_at));

    print_fetch_rows(user_at, sep, os_line, shell_line, ram_line, cpu_line);

    return 0;
}
