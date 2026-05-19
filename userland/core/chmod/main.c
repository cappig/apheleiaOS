#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(void) {
    io_write_str("usage: chmod MODE FILE...\n");
}

static int parse_octal_mode(const char *text, mode_t *out) {
    if (!text || !*text || !out) {
        return -1;
    }

    char *end = NULL;
    long value = strtol(text, &end, 8);

    if (!end || *end != '\0') {
        return -1;
    }

    if (value < 0 || value > 07777L) {
        return -1;
    }

    *out = (mode_t)value;
    return 0;
}

static bool is_symbolic_mode(const char *text) {
    if (!text || !*text) {
        return false;
    }

    for (const char *cursor = text; *cursor; cursor++) {
        if (*cursor == '+' || *cursor == '-' || *cursor == '=') {
            return true;
        }
    }

    return false;
}

static mode_t who_clear_mask(unsigned who) {
    mode_t mask = 0;

    if (who & 0x1) {
        mask |= (S_IRUSR | S_IWUSR | S_IXUSR | S_ISUID);
    }
    if (who & 0x2) {
        mask |= (S_IRGRP | S_IWGRP | S_IXGRP | S_ISGID);
    }
    if (who & 0x4) {
        mask |= (S_IROTH | S_IWOTH | S_IXOTH | S_ISVTX);
    }

    return mask;
}

static mode_t perm_bits_for(unsigned who, char perm, mode_t base_mode) {
    bool x_ok = 
        (base_mode & S_IFMT) == S_IFDIR || (base_mode & (S_IXUSR | S_IXGRP | S_IXOTH));

    mode_t bits = 0;

    if (perm == 'r') {
        if (who & 0x1) {
            bits |= S_IRUSR;
        }
        if (who & 0x2) {
            bits |= S_IRGRP;
        }
        if (who & 0x4) {
            bits |= S_IROTH;
        }
        return bits;
    }

    if (perm == 'w') {
        if (who & 0x1) {
            bits |= S_IWUSR;
        }
        if (who & 0x2) {
            bits |= S_IWGRP;
        }
        if (who & 0x4) {
            bits |= S_IWOTH;
        }
        return bits;
    }

    if (perm == 'x' || (perm == 'X' && x_ok)) {
        if (who & 0x1) {
            bits |= S_IXUSR;
        }
        if (who & 0x2) {
            bits |= S_IXGRP;
        }
        if (who & 0x4) {
            bits |= S_IXOTH;
        }
        return bits;
    }

    if (perm == 's') {
        if (who & 0x1) {
            bits |= S_ISUID;
        }
        if (who & 0x2) {
            bits |= S_ISGID;
        }
        return bits;
    }

    if (perm == 't') {
        if (who & 0x4) {
            bits |= S_ISVTX;
        }
    }

    return bits;
}

static int apply_symbolic_mode(const char *text, mode_t start_mode, mode_t *out) {
    if (!text || !*text || !out) {
        return -1;
    }

    mode_t mode = start_mode;
    const char *cursor = text;

    while (*cursor) {
        unsigned who = 0;
        while (
            *cursor == 'u' ||
            *cursor == 'g' ||
            *cursor == 'o' ||
            *cursor == 'a'
        ) {
            if (*cursor == 'u' || *cursor == 'a') {
                who |= 0x1;
            }
            if (*cursor == 'g' || *cursor == 'a') {
                who |= 0x2;
            }
            if (*cursor == 'o' || *cursor == 'a') {
                who |= 0x4;
            }
            cursor++;
        }

        if (!who) {
            who = 0x1 | 0x2 | 0x4;
        }

        char op = *cursor;
        if (op != '+' && op != '-' && op != '=') {
            return -1;
        }

        cursor++;

        mode_t bits = 0;
        bool have_perm = false;

        while (
            *cursor == 'r' ||
            *cursor == 'w' ||
            *cursor == 'x' ||
            *cursor == 'X' ||
            *cursor == 's' ||
            *cursor == 't'
        ) {
            bits |= perm_bits_for(who, *cursor, mode);
            have_perm = true;
            cursor++;
        }

        if (!have_perm && op != '=') {
            return -1;
        }

        if (op == '+') {
            mode |= bits;
        } else if (op == '-') {
            mode &= (mode_t)~bits;
        } else {
            mode &= (mode_t)~who_clear_mask(who);
            mode |= bits;
        }

        if (!*cursor) {
            break;
        }

        if (*cursor != ',') {
            return -1;
        }

        cursor++;
        if (!*cursor) {
            return -1;
        }
    }

    *out = mode;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage();
        return 1;
    }

    bool symbolic = is_symbolic_mode(argv[1]);
    mode_t mode = 0;

    if (!symbolic && parse_octal_mode(argv[1], &mode) != 0) {
        io_write_str("chmod: invalid mode\n");
        return 1;
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        mode_t target_mode = mode;
        if (symbolic) {
            struct stat st = {0};

            if (stat(argv[i], &st) != 0) {
                char msg[128];
                snprintf(
                    msg,
                    sizeof(msg),
                    "chmod: %s: %s\n",
                    argv[i],
                    strerror(errno)
                );
                io_write_str(msg);
                rc = 1;
                continue;
            }

            if (apply_symbolic_mode(argv[1], st.st_mode, &target_mode) != 0) {
                io_write_str("chmod: invalid mode\n");
                rc = 1;
                continue;
            }
        }

        if (chmod(argv[i], target_mode) != 0) {
            char msg[128];
            snprintf(
                msg, sizeof(msg), "chmod: %s: %s\n", argv[i], strerror(errno)
            );
            io_write_str(msg);
            rc = 1;
        }
    }

    return rc;
}
