#include <kv.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
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

static void format_ram_line(
    unsigned long long used_kib,
    unsigned long long total_kib,
    char* out,
    size_t out_len
) {
    if (!out || !out_len) {
        return;
    }

    if (!total_kib) {
        snprintf(out, out_len, "ram: <unknown>");
        return;
    }

    unsigned long long unit_kib = 1;
    const char* unit = "KiB";

    if (total_kib >= 1024ULL * 1024ULL) {
        unit_kib = 1024ULL * 1024ULL;
        unit = "GiB";
    } else if (total_kib >= 1024ULL) {
        unit_kib = 1024ULL;
        unit = "MiB";
    }

    unsigned long long used = used_kib / unit_kib;
    unsigned long long total = total_kib / unit_kib;

    snprintf(out, out_len, "ram: %llu %s / %llu %s", used, unit, total, unit);
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
    unsigned long long used_kib = 0;
    unsigned long long freq_khz = 0;

    char os_kv[256] = {0};
    char swap_kv[256] = {0};
    char cpu_kv[256] = {0};

    if (kv_read_file("/dev/os", os_kv, sizeof(os_kv)) > 0) {
        kv_read_string(os_kv, "name", os_name, sizeof(os_name));
        kv_read_string(os_kv, "arch", os_arch, sizeof(os_arch));
    }

    if (kv_read_file("/dev/swap", swap_kv, sizeof(swap_kv)) > 0) {
        kv_read_u64(swap_kv, "total_kib", &total_kib);
        kv_read_u64(swap_kv, "used_kib", &used_kib);
    }

    if (kv_read_file("/dev/cpu", cpu_kv, sizeof(cpu_kv)) > 0) {
        kv_read_string(cpu_kv, "model", cpu_model, sizeof(cpu_model));
        kv_read_u64(cpu_kv, "clockrate_khz", &freq_khz);
    }

    snprintf(user_at, sizeof(user_at), "%s@%s", user, os_name);
    snprintf(os_line, sizeof(os_line), "os: %s %s", os_name, os_arch);
    snprintf(shell_line, sizeof(shell_line), "shell: %s", shell);

    format_ram_line(used_kib, total_kib, ram_line, sizeof(ram_line));

    if (freq_khz > 0)
        snprintf(cpu_line, sizeof(cpu_line), "cpu: %s @ %llu MHz", cpu_model, freq_khz / 1000);
    else
        snprintf(cpu_line, sizeof(cpu_line), "cpu: %s", cpu_model);

    fill_separator(sep, sizeof(sep), strlen(user_at));

    print_fetch_rows(user_at, sep, os_line, shell_line, ram_line, cpu_line);

    return 0;
}
