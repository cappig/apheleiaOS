#include <crypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CRYPT_PREFIX "fnv1a$"
#define FNV1A_OFFSET 2166136261u
#define FNV1A_PRIME  16777619u

// toy password hash, not suitable for real authentication
char *crypt(const char *key, const char *salt) {
    static char buf[64];

    if (!key) {
        return NULL;
    }

    if (!salt || strncmp(salt, CRYPT_PREFIX, strlen(CRYPT_PREFIX)) != 0) {
        salt = CRYPT_PREFIX;
    }

    uint32_t hash = FNV1A_OFFSET;

    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        hash ^= *p;
        hash *= FNV1A_PRIME;
    }

    int written = snprintf(buf, sizeof(buf), CRYPT_PREFIX "%08x", (unsigned int)hash);
    if (written <= 0) {
        return NULL;
    }

    buf[sizeof(buf) - 1] = '\0';
    return buf;
}
