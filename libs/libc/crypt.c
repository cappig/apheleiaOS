#include <crypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CRYPT_PREFIX "fnv1a$"

// WARN: simple non crytographic hash for the simple toy os :^)
char *crypt(const char *key, const char *salt) {
    static char buf[64];

    if (!key) {
        return NULL;
    }

    if (!salt || strncmp(salt, CRYPT_PREFIX, strlen(CRYPT_PREFIX)) != 0) {
        salt = CRYPT_PREFIX;
    }

    const uint32_t fnv_offset = 2166136261u;
    const uint32_t fnv_prime = 16777619u;
    uint32_t hash = fnv_offset;

    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        hash ^= *p;
        hash *= fnv_prime;
    }

    int written = snprintf(buf, sizeof(buf), CRYPT_PREFIX "%08x", (unsigned int)hash);
    if (written <= 0) {
        return NULL;
    }

    buf[sizeof(buf) - 1] = '\0';
    return buf;
}
