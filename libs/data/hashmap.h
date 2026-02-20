#pragma once

#include <base/types.h>
#include <stddef.h>

#define HASHMAP_INITIAL_CAPACITY 16

typedef struct hashmap_entry {
    u64 key;
    u64 value;
    u8 state;
} hashmap_entry_t;

typedef struct hashmap {
    size_t size;
    size_t capacity;
    hashmap_entry_t *entries;
} hashmap_t;

u64 hashmap_hash_u64(u64 value);
u64 hashmap_hash_bytes(const void *data, size_t len);
u64 hashmap_hash_str(const char *text);

typedef u64 (*hashmap_str_hash_fn)(const char *text, void *private_data);
typedef bool (*hashmap_str_cmp_fn)(const char *left, const char *right, void *private_data);

typedef struct hashmap_str_entry {
    const char *key;
    u64 value;
    struct hashmap_str_entry *next;
} hashmap_str_entry_t;

typedef struct hashmap_str {
    hashmap_t *index;
    size_t size;
    hashmap_str_hash_fn hash_fn;
    hashmap_str_cmp_fn cmp_fn;
    void *private_data;
} hashmap_str_t;

hashmap_t *hashmap_create(void);
hashmap_t *hashmap_create_sized(size_t capacity);

void hashmap_destroy(hashmap_t *map);
void hashmap_clear(hashmap_t *map);

bool hashmap_reserve(hashmap_t *map, size_t capacity);

bool hashmap_set(hashmap_t *map, u64 key, u64 value);
bool hashmap_get(const hashmap_t *map, u64 key, u64 *value_out);
bool hashmap_remove(hashmap_t *map, u64 key);

hashmap_str_t *hashmap_str_create(void);
hashmap_str_t *hashmap_str_create_with(
    hashmap_str_hash_fn hash_fn,
    hashmap_str_cmp_fn cmp_fn,
    void *private_data
);

void hashmap_str_destroy(hashmap_str_t *map);
void hashmap_str_clear(hashmap_str_t *map);
size_t hashmap_str_size(const hashmap_str_t *map);

bool hashmap_str_set(hashmap_str_t *map, const char *key, u64 value);
bool hashmap_str_get(const hashmap_str_t *map, const char *key, u64 *value_out);
bool hashmap_str_remove(hashmap_str_t *map, const char *key);
