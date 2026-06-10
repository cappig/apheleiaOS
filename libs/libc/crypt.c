#include <crypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CRYPT_PREFIX       "fnv1a64$"
#define CRYPT_DEFAULT_SALT "apheleia"
#define CRYPT_SALT_MAX     32
#define FNV_OFFSET         14695981039346656037ULL
#define FNV_PRIME          1099511628211ULL

static uint64_t fnv_mix(uint64_t hash, unsigned char value) {
    hash ^= value;
    hash *= FNV_PRIME;
    return hash;
}

static uint64_t fnv_string(uint64_t hash, const char *text) {
    while (text && *text) {
        hash = fnv_mix(hash, (unsigned char)*text++);
    }

    return hash;
}

static uint64_t password_hash(const char *key, const char *salt) {
    uint64_t hash = FNV_OFFSET;

    hash = fnv_string(hash, salt);
    hash = fnv_mix(hash, '$');
    hash = fnv_string(hash, key);

    return hash;
}

static int salt_char_ok(char c) {
    if (c >= 'a' && c <= 'z') {
        return 1;
    }

    if (c >= 'A' && c <= 'Z') {
        return 1;
    }

    if (c >= '0' && c <= '9') {
        return 1;
    }

    return c == '.' || c == '_' || c == '-';
}

static void copy_salt(const char *salt, char *out, size_t cap) {
    if (!salt || !salt[0]) {
        salt = CRYPT_DEFAULT_SALT;
    }

    if (strncmp(salt, CRYPT_PREFIX, strlen(CRYPT_PREFIX)) == 0) {
        salt += strlen(CRYPT_PREFIX);
    }

    size_t len = 0;
    while (salt[len] && salt[len] != '$' && len + 1 < cap && salt_char_ok(salt[len])) {
        out[len] = salt[len];
        len++;
    }

    if (!len) {
        strncpy(out, CRYPT_DEFAULT_SALT, cap - 1);
        out[cap - 1] = '\0';
        return;
    }

    out[len] = '\0';
}

char *crypt(const char *key, const char *salt) {
    static char buf[96];

    if (!key) {
        return NULL;
    }

    char salt_buf[CRYPT_SALT_MAX + 1] = { 0 };
    copy_salt(salt, salt_buf, sizeof(salt_buf));

    uint64_t hash = password_hash(key, salt_buf);
    int written = snprintf(buf, sizeof(buf), CRYPT_PREFIX "%s$%016llx", salt_buf, (unsigned long long)hash);

    if (written <= 0 || (size_t)written >= sizeof(buf)) {
        return NULL;
    }

    return buf;
}
