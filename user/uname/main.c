#include <kv.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char name[32];
    char release[32];
    char version[96];
    char arch[32];
} uname_info_t;

static void load_info(uname_info_t* info) {
    if (!info)
        return;

    snprintf(info->name, sizeof(info->name), "unknown");
    snprintf(info->release, sizeof(info->release), "unknown");
    snprintf(info->version, sizeof(info->version), "unknown");
    snprintf(info->arch, sizeof(info->arch), "unknown");

    char os_text[256] = {0};
    if (kv_read_file("/dev/os", os_text, sizeof(os_text)) <= 0)
        return;

    kv_read_string(os_text, "name", info->name, sizeof(info->name));
    kv_read_string(os_text, "release", info->release, sizeof(info->release));
    kv_read_string(os_text, "version", info->version, sizeof(info->version));
    kv_read_string(os_text, "arch", info->arch, sizeof(info->arch));
}

static void print_field(const char* text, bool* first) {
    if (!text || !first)
        return;

    if (!*first)
        write(STDOUT_FILENO, " ", 1);

    write(STDOUT_FILENO, text, strlen(text));
    *first = false;
}

int main(int argc, char** argv) {
    bool opt_s = false;
    bool opt_r = false;
    bool opt_v = false;
    bool opt_m = false;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (!arg || arg[0] != '-' || !arg[1]) {
            write(STDOUT_FILENO, "usage: uname [-amrsv]\n", 22);
            return 1;
        }

        for (size_t j = 1; arg[j]; j++) {
            switch (arg[j]) {
            case 'a':
                opt_s = true;
                opt_r = true;
                opt_v = true;
                opt_m = true;
                break;
            case 's':
                opt_s = true;
                break;
            case 'r':
                opt_r = true;
                break;
            case 'v':
                opt_v = true;
                break;
            case 'm':
                opt_m = true;
                break;
            default:
                write(STDOUT_FILENO, "usage: uname [-amrsv]\n", 22);
                return 1;
            }
        }
    }

    if (!opt_s && !opt_r && !opt_v && !opt_m)
        opt_s = true;

    uname_info_t info = {0};
    load_info(&info);

    bool first = true;

    if (opt_s)
        print_field(info.name, &first);
    if (opt_r)
        print_field(info.release, &first);
    if (opt_v)
        print_field(info.version, &first);
    if (opt_m)
        print_field(info.arch, &first);

    write(STDOUT_FILENO, "\n", 1);
    return 0;
}
