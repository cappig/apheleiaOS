#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ATEXIT_MAX_FUNCS 32

static void (*atexit_funcs[ATEXIT_MAX_FUNCS])(void);
static size_t atexit_count = 0;

static bool env_owned = false;
static size_t env_count = 0;

static long double _ld_abs(long double value) {
    return value < 0.0L ? -value : value;
}

static int _env_name_valid(const char *name) {
    if (!name || !name[0]) {
        return 0;
    }

    for (const char *cursor = name; *cursor; cursor++) {
        if (*cursor == '=') {
            return 0;
        }
    }

    return 1;
}

static size_t _count_env(char **env) {
    size_t count = 0;
    if (!env) {
        return 0;
    }

    while (env[count]) {
        count++;
    }
    return count;
}

static int _ensure_owned_env(void) {
    if (env_owned) {
        return 0;
    }

    size_t count = _count_env(environ);
    char **copy = calloc(count + 1, sizeof(char *));
    if (!copy) {
        errno = ENOMEM;
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        copy[i] = strdup(environ[i]);
        if (!copy[i]) {
            for (size_t j = 0; j < i; j++) {
                free(copy[j]);
            }
            free(copy);
            errno = ENOMEM;
            return -1;
        }
    }

    copy[count] = NULL;
    environ = copy;
    env_count = count;
    env_owned = true;
    return 0;
}

static int _env_match(const char *entry, const char *name, size_t name_len) {
    return
        entry &&
        !strncmp(entry, name, name_len) &&
        entry[name_len] == '=';
}

static int _find_env_index(const char *name) {
    if (!environ || !name) {
        return -1;
    }

    size_t name_len = strlen(name);
    for (size_t i = 0; i < env_count; i++) {
        if (_env_match(environ[i], name, name_len)) {
            return (int)i;
        }
    }

    return -1;
}

int atexit(void (*fn)(void)) {
    if (!fn) {
        errno = EINVAL;
        return -1;
    }

    if (atexit_count >= ATEXIT_MAX_FUNCS) {
        errno = ENOMEM;
        return -1;
    }

    atexit_funcs[atexit_count++] = fn;
    return 0;
}

void _Exit(int status) {
    _exit(status);

    for (;;) {
        ;
    }
}

void exit(int status) {
    while (atexit_count) {
        void (*fn)(void) = atexit_funcs[--atexit_count];
        if (fn) {
            fn();
        }
    }

    _Exit(status);
}

char *getenv(const char *name) {
    if (!_env_name_valid(name)) {
        return NULL;
    }

    size_t name_len = strlen(name);
    if (!environ) {
        return NULL;
    }

    for (size_t i = 0; environ[i]; i++) {
        if (_env_match(environ[i], name, name_len)) {
            return environ[i] + name_len + 1;
        }
    }

    return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!_env_name_valid(name) || !value) {
        errno = EINVAL;
        return -1;
    }

    if (_ensure_owned_env() < 0) {
        return -1;
    }

    int idx = _find_env_index(name);
    if (idx >= 0 && !overwrite) {
        return 0;
    }

    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    char *entry = malloc(name_len + 1 + value_len + 1);
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }

    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    memcpy(entry + name_len + 1, value, value_len + 1);

    if (idx >= 0) {
        free(environ[idx]);
        environ[idx] = entry;
        return 0;
    }

    char **new_env = realloc(environ, (env_count + 2) * sizeof(char *));
    if (!new_env) {
        free(entry);
        errno = ENOMEM;
        return -1;
    }

    environ = new_env;
    environ[env_count++] = entry;
    environ[env_count] = NULL;
    return 0;
}

int unsetenv(const char *name) {
    if (!_env_name_valid(name)) {
        errno = EINVAL;
        return -1;
    }

    if (_ensure_owned_env() < 0) {
        return -1;
    }

    size_t name_len = strlen(name);
    size_t dst = 0;

    for (size_t src = 0; src < env_count; src++) {
        if (_env_match(environ[src], name, name_len)) {
            free(environ[src]);
            continue;
        }

        environ[dst++] = environ[src];
    }

    env_count = dst;
    environ[env_count] = NULL;
    return 0;
}

static int _append_component(char *out, size_t out_cap, const char *comp) {
    if (!out || !comp || !out_cap) {
        return -1;
    }

    size_t out_len = strlen(out);
    size_t comp_len = strlen(comp);

    if (
        out_len + (out_len > 1 ? 1 : 0) + comp_len + 1 > out_cap
    ) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (out_len > 1 && out[out_len - 1] != '/') {
        out[out_len++] = '/';
    }

    memcpy(out + out_len, comp, comp_len + 1);
    return 0;
}

static void _pop_component(char *out) {
    size_t len = strlen(out);
    if (len <= 1) {
        strcpy(out, "/");
        return;
    }

    while (len > 1 && out[len - 1] == '/') {
        out[--len] = '\0';
    }

    while (len > 1 && out[len - 1] != '/') {
        out[--len] = '\0';
    }

    if (!len) {
        strcpy(out, "/");
    }
}

char *realpath(const char *path, char *resolved_path) {
    if (!path || !path[0]) {
        errno = EINVAL;
        return NULL;
    }

    char combined[PATH_MAX] = {0};
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(combined)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        strcpy(combined, path);
    } else {
        if (!getcwd(combined, sizeof(combined))) {
            return NULL;
        }

        size_t len = strlen(combined);
        if (len + 1 + strlen(path) + 1 > sizeof(combined)) {
            errno = ENAMETOOLONG;
            return NULL;
        }

        if (len > 1 && combined[len - 1] != '/') {
            combined[len++] = '/';
            combined[len] = '\0';
        }

        strcat(combined, path);
    }

    char work[PATH_MAX] = {0};
    strcpy(work, combined);

    char normalized[PATH_MAX] = "/";
    char *save = NULL;
    char *tok = strtok_r(work, "/", &save);
    while (tok) {
        if (!strcmp(tok, ".") || !tok[0]) {
            tok = strtok_r(NULL, "/", &save);
            continue;
        }

        if (!strcmp(tok, "..")) {
            _pop_component(normalized);
            tok = strtok_r(NULL, "/", &save);
            continue;
        }

        if (_append_component(normalized, sizeof(normalized), tok) < 0) {
            return NULL;
        }

        tok = strtok_r(NULL, "/", &save);
    }

    struct stat st = {0};
    if (stat(normalized, &st) < 0) {
        return NULL;
    }

    char *out = resolved_path;
    if (!out) {
        out = malloc(PATH_MAX);
        if (!out) {
            errno = ENOMEM;
            return NULL;
        }
    }

    if (strlen(normalized) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        if (!resolved_path) {
            free(out);
        }
        return NULL;
    }

    strcpy(out, normalized);
    return out;
}

long double strtold(char const *restrict str, char **restrict endptr) {
    if (!str) {
        errno = EINVAL;
        if (endptr) {
            *endptr = NULL;
        }
        return 0.0L;
    }

    const char *cursor = str;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    int sign = 1;
    if (*cursor == '+' || *cursor == '-') {
        if (*cursor == '-') {
            sign = -1;
        }
        cursor++;
    }

    long double value = 0.0L;
    bool has_digits = false;

    while (isdigit((unsigned char)*cursor)) {
        has_digits = true;
        value = value * 10.0L + (long double)(*cursor - '0');
        cursor++;
    }

    if (*cursor == '.') {
        long double scale = 0.1L;
        cursor++;

        while (isdigit((unsigned char)*cursor)) {
            has_digits = true;
            value += (long double)(*cursor - '0') * scale;
            scale *= 0.1L;
            cursor++;
        }
    }

    if (!has_digits) {
        if (endptr) {
            *endptr = (char *)str;
        }
        return 0.0L;
    }

    int exp_sign = 1;
    int exp_value = 0;

    const char *exp_mark = cursor;
    if (*cursor == 'e' || *cursor == 'E') {
        cursor++;

        if (*cursor == '+' || *cursor == '-') {
            if (*cursor == '-') {
                exp_sign = -1;
            }
            cursor++;
        }

        const char *exp_start = cursor;
        while (isdigit((unsigned char)*cursor)) {
            if (exp_value < 1000000) {
                exp_value = exp_value * 10 + (*cursor - '0');
            }
            cursor++;
        }

        if (cursor == exp_start) {
            cursor = exp_mark;
            exp_value = 0;
            exp_sign = 1;
        }
    }

    int exponent = exp_sign * exp_value;
    bool overflow = false;
    bool underflow = false;

    if (exponent > 0) {
        for (int i = 0; i < exponent; i++) {
            if (value > LDBL_MAX / 10.0L) {
                value = LDBL_MAX;
                overflow = true;
                break;
            }

            value *= 10.0L;
        }
    } else if (exponent < 0) {
        for (int i = exponent; i < 0; i++) {
            value /= 10.0L;
            if (value != 0.0L && _ld_abs(value) < LDBL_MIN) {
                underflow = true;
            }
        }
    }

    if (overflow || underflow) {
        errno = ERANGE;
    }

    if (endptr) {
        *endptr = (char *)cursor;
    }

    return sign < 0 ? -value : value;
}

double strtod(char const *restrict str, char **restrict endptr) {
    long double value = strtold(str, endptr);
    long double abs_value = _ld_abs(value);

    if (abs_value > (long double)DBL_MAX) {
        errno = ERANGE;
        return value < 0 ? -DBL_MAX : DBL_MAX;
    }

    if (abs_value != 0.0L && abs_value < (long double)DBL_MIN) {
        errno = ERANGE;
    }

    return (double)value;
}

float strtof(char const *restrict str, char **restrict endptr) {
    long double value = strtold(str, endptr);
    long double abs_value = _ld_abs(value);

    if (abs_value > (long double)FLT_MAX) {
        errno = ERANGE;
        return value < 0 ? -FLT_MAX : FLT_MAX;
    }

    if (abs_value != 0.0L && abs_value < (long double)FLT_MIN) {
        errno = ERANGE;
    }

    return (float)value;
}

double atof(char const *str) {
    return strtod(str, NULL);
}

int system(const char *command) {
    if (!command) {
        return access("/bin/sh", X_OK) == 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (!pid) {
        char *argv[] = {"/bin/sh", "-c", (char *)command, NULL};
        execve("/bin/sh", argv, environ);
        _Exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return status;
}
