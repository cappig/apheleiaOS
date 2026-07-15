#include "test.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool parse_int(const char *text, long *value) {
    if (!text || !value) {
        return false;
    }

    char *end = NULL;
    *value = strtol(text, &end, 10);
    return end && end != text && !*end;
}

static bool eval_file(const char *op, const char *path, bool *valid) {
    *valid = true;

    if (!strcmp(op, "-r")) {
        return access(path, R_OK) == 0;
    }
    if (!strcmp(op, "-w")) {
        return access(path, W_OK) == 0;
    }
    if (!strcmp(op, "-x")) {
        return access(path, X_OK) == 0;
    }
    if (!strcmp(op, "-e")) {
        return access(path, F_OK) == 0;
    }

    bool regular = !strcmp(op, "-f");
    bool directory = !strcmp(op, "-d");
    bool nonempty = !strcmp(op, "-s");
    if (!regular && !directory && !nonempty) {
        *valid = false;
        return false;
    }

    struct stat st;
    if (stat(path, &st) < 0) {
        return false;
    }
    if (regular) {
        return (st.st_mode & S_IFMT) == S_IFREG;
    }
    if (directory) {
        return (st.st_mode & S_IFMT) == S_IFDIR;
    }
    return st.st_size > 0;
}

static bool eval_unary(const char *op, const char *value, bool *valid) {
    if (!strcmp(op, "-n")) {
        *valid = true;
        return value[0] != '\0';
    }
    if (!strcmp(op, "-z")) {
        *valid = true;
        return value[0] == '\0';
    }

    return eval_file(op, value, valid);
}

static bool eval_binary(const char *left, const char *op, const char *right, bool *valid) {
    *valid = true;
    if (!strcmp(op, "=")) {
        return !strcmp(left, right);
    }
    if (!strcmp(op, "!=")) {
        return strcmp(left, right) != 0;
    }

    long a = 0;
    long b = 0;
    if (!parse_int(left, &a) || !parse_int(right, &b)) {
        *valid = false;
        return false;
    }
    if (!strcmp(op, "-eq")) {
        return a == b;
    }
    if (!strcmp(op, "-ne")) {
        return a != b;
    }
    if (!strcmp(op, "-gt")) {
        return a > b;
    }
    if (!strcmp(op, "-ge")) {
        return a >= b;
    }
    if (!strcmp(op, "-lt")) {
        return a < b;
    }
    if (!strcmp(op, "-le")) {
        return a <= b;
    }

    *valid = false;
    return false;
}

static bool eval_expr(int argc, char *const argv[], bool *valid) {
    *valid = true;
    if (!argc) {
        return false;
    }
    if (argc > 1 && !strcmp(argv[0], "!")) {
        return !eval_expr(argc - 1, argv + 1, valid);
    }

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) {
            bool left_valid = false;
            bool right_valid = false;
            bool left = eval_expr(i, argv, &left_valid);
            bool right = eval_expr(argc - i - 1, argv + i + 1, &right_valid);
            *valid = left_valid && right_valid;
            return left || right;
        }
    }

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) {
            bool left_valid = false;
            bool right_valid = false;
            bool left = eval_expr(i, argv, &left_valid);
            bool right = eval_expr(argc - i - 1, argv + i + 1, &right_valid);
            *valid = left_valid && right_valid;
            return left && right;
        }
    }

    if (argc == 1) {
        return argv[0][0] != '\0';
    }
    if (argc == 2) {
        return eval_unary(argv[0], argv[1], valid);
    }
    if (argc == 3) {
        return eval_binary(argv[0], argv[1], argv[2], valid);
    }

    *valid = false;
    return false;
}

int test_run(int argc, char *const argv[], const char **error) {
    if (error) {
        *error = NULL;
    }
    if (argc <= 0 || !argv || !argv[0]) {
        return 2;
    }

    int count = argc - 1;
    if (!strcmp(argv[0], "[")) {
        if (!count || strcmp(argv[argc - 1], "]")) {
            if (error) {
                *error = "missing ]";
            }
            return 2;
        }
        count--;
    }

    bool valid = false;
    bool value = eval_expr(count, argv + 1, &valid);
    if (!valid) {
        if (error) {
            *error = "invalid expression";
        }
        return 2;
    }

    return value ? 0 : 1;
}
