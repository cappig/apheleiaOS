#include "hashmap.h"

#include <stdlib.h>
#include <string.h>

enum {
    HASHMAP_SLOT_EMPTY = 0,
    HASHMAP_SLOT_FULL = 1,
    HASHMAP_SLOT_TOMBSTONE = 2,
};

#define HASHMAP_LOAD_NUM 7
#define HASHMAP_LOAD_DEN 10

u64 hashmap_hash_u64(u64 value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

u64 hashmap_hash_bytes(const void *data, size_t len) {
    if (!data && len) {
        return 0;
    }

    const u8 *bytes = data;
    u64 hash = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }

    return hash;
}

u64 hashmap_hash_str(const char *text) {
    if (!text) {
        return 0;
    }

    return hashmap_hash_bytes(text, strlen(text));
}

static u64 _hashmap_str_hash_default(const char *text, void *private_data) {
    (void)private_data;
    return hashmap_hash_str(text);
}

static bool _hashmap_str_cmp_default(const char *left, const char *right, void *private_data) {
    (void)private_data;
    return left && right && !strcmp(left, right);
}

static bool _is_power_of_two(size_t value) {
    return value && !(value & (value - 1));
}

static size_t _next_power_of_two(size_t value) {
    if (value <= 1) {
        return 1;
    }

    size_t rounded = 1;
    while (rounded < value) {
        size_t grown = rounded << 1;
        if (grown <= rounded) {
            return rounded;
        }
        rounded = grown;
    }

    return rounded;
}

static bool _hashmap_insert_raw(hashmap_t *map, u64 key, u64 value) {
    if (!map || !map->entries || !map->capacity) {
        return false;
    }

    size_t mask = map->capacity - 1;
    size_t index = (size_t)hashmap_hash_u64(key) & mask;
    size_t start = index;
    size_t tombstone = (size_t)-1;

    for (;;) {
        hashmap_entry_t *entry = &map->entries[index];

        if (entry->state == HASHMAP_SLOT_FULL) {
            if (entry->key == key) {
                entry->value = value;
                return true;
            }
        } else if (entry->state == HASHMAP_SLOT_EMPTY) {
            size_t target = tombstone != (size_t)-1 ? tombstone : index;
            entry = &map->entries[target];
            entry->state = HASHMAP_SLOT_FULL;
            entry->key = key;
            entry->value = value;
            map->size++;
            return true;
        } else if (entry->state == HASHMAP_SLOT_TOMBSTONE) {
            if (tombstone == (size_t)-1) {
                tombstone = index;
            }
        }

        index = (index + 1) & mask;
        if (index == start) {
            break;
        }
    }

    if (tombstone != (size_t)-1) {
        hashmap_entry_t *entry = &map->entries[tombstone];
        entry->state = HASHMAP_SLOT_FULL;
        entry->key = key;
        entry->value = value;
        map->size++;
        return true;
    }

    return false;
}

static bool _hashmap_update_existing(hashmap_t *map, u64 key, u64 value) {
    if (!map || !map->entries || !map->capacity) {
        return false;
    }

    size_t mask = map->capacity - 1;
    size_t index = (size_t)hashmap_hash_u64(key) & mask;
    size_t start = index;

    for (;;) {
        hashmap_entry_t *entry = &map->entries[index];

        if (entry->state == HASHMAP_SLOT_EMPTY) {
            return false;
        }

        if (entry->state == HASHMAP_SLOT_FULL && entry->key == key) {
            entry->value = value;
            return true;
        }

        index = (index + 1) & mask;
        if (index == start) {
            return false;
        }
    }
}

static bool _hashmap_rehash(hashmap_t *map, size_t new_capacity) {
    if (!map) {
        return false;
    }

    if (!_is_power_of_two(new_capacity)) {
        new_capacity = _next_power_of_two(new_capacity);
    }
    if (!new_capacity) {
        return false;
    }

    hashmap_entry_t *new_entries = calloc(new_capacity, sizeof(*new_entries));
    if (!new_entries) {
        return false;
    }

    hashmap_entry_t *old_entries = map->entries;
    size_t old_capacity = map->capacity;

    map->entries = new_entries;
    map->capacity = new_capacity;
    map->size = 0;

    for (size_t i = 0; i < old_capacity; i++) {
        const hashmap_entry_t *entry = &old_entries[i];
        if (entry->state != HASHMAP_SLOT_FULL) {
            continue;
        }

        if (!_hashmap_insert_raw(map, entry->key, entry->value)) {
            free(old_entries);
            return false;
        }
    }

    free(old_entries);
    return true;
}

hashmap_t *hashmap_create(void) {
    return hashmap_create_sized(HASHMAP_INITIAL_CAPACITY);
}

hashmap_t *hashmap_create_sized(size_t capacity) {
    if (!capacity) {
        capacity = HASHMAP_INITIAL_CAPACITY;
    }

    if (!_is_power_of_two(capacity)) {
        capacity = _next_power_of_two(capacity);
    }

    hashmap_t *map = calloc(1, sizeof(*map));
    if (!map) {
        return NULL;
    }

    map->entries = calloc(capacity, sizeof(*map->entries));
    if (!map->entries) {
        free(map);
        return NULL;
    }

    map->capacity = capacity;
    return map;
}

void hashmap_destroy(hashmap_t *map) {
    if (!map) {
        return;
    }

    free(map->entries);
    free(map);
}

void hashmap_clear(hashmap_t *map) {
    if (!map || !map->entries || !map->capacity) {
        return;
    }

    memset(map->entries, 0, map->capacity * sizeof(*map->entries));
    map->size = 0;
}

bool hashmap_reserve(hashmap_t *map, size_t capacity) {
    if (!map) {
        return false;
    }

    size_t min_capacity = capacity;
    if (map->size > 0) {
        size_t needed = (map->size * HASHMAP_LOAD_DEN + HASHMAP_LOAD_NUM - 1) / HASHMAP_LOAD_NUM;
        if (needed > min_capacity) {
            min_capacity = needed;
        }
    }

    if (min_capacity <= map->capacity) {
        return true;
    }

    return _hashmap_rehash(map, min_capacity);
}

bool hashmap_set(hashmap_t *map, u64 key, u64 value) {
    if (!map) {
        return false;
    }

    if (!map->entries || !map->capacity) {
        if (!_hashmap_rehash(map, HASHMAP_INITIAL_CAPACITY)) {
            return false;
        }
    }

    if (_hashmap_update_existing(map, key, value)) {
        return true;
    }

    size_t limit = (map->capacity * HASHMAP_LOAD_NUM) / HASHMAP_LOAD_DEN;
    if (map->size + 1 > limit) {
        size_t next = map->capacity ? map->capacity << 1 : HASHMAP_INITIAL_CAPACITY;
        if (!_hashmap_rehash(map, next)) {
            return false;
        }
    }

    if (_hashmap_insert_raw(map, key, value)) {
        return true;
    }

    size_t next = map->capacity ? map->capacity << 1 : HASHMAP_INITIAL_CAPACITY;
    if (!_hashmap_rehash(map, next)) {
        return false;
    }

    return _hashmap_insert_raw(map, key, value);
}

bool hashmap_get(const hashmap_t *map, u64 key, u64 *value_out) {
    if (!map || !map->entries || !map->capacity) {
        return false;
    }

    size_t mask = map->capacity - 1;
    size_t index = (size_t)hashmap_hash_u64(key) & mask;
    size_t start = index;

    for (;;) {
        const hashmap_entry_t *entry = &map->entries[index];

        if (entry->state == HASHMAP_SLOT_EMPTY) {
            return false;
        }

        if (entry->state == HASHMAP_SLOT_FULL && entry->key == key) {
            if (value_out) {
                *value_out = entry->value;
            }
            return true;
        }

        index = (index + 1) & mask;
        if (index == start) {
            return false;
        }
    }
}

bool hashmap_remove(hashmap_t *map, u64 key) {
    if (!map || !map->entries || !map->capacity) {
        return false;
    }

    size_t mask = map->capacity - 1;
    size_t index = (size_t)hashmap_hash_u64(key) & mask;
    size_t start = index;

    for (;;) {
        hashmap_entry_t *entry = &map->entries[index];

        if (entry->state == HASHMAP_SLOT_EMPTY) {
            return false;
        }

        if (entry->state == HASHMAP_SLOT_FULL && entry->key == key) {
            entry->state = HASHMAP_SLOT_TOMBSTONE;
            entry->key = 0;
            entry->value = 0;
            if (map->size) {
                map->size--;
            }
            return true;
        }

        index = (index + 1) & mask;
        if (index == start) {
            return false;
        }
    }
}

static hashmap_str_entry_t *_hashmap_str_bucket_head(const hashmap_str_t *map, u64 hash) {
    if (!map || !map->index) {
        return NULL;
    }

    u64 encoded = 0;
    if (!hashmap_get(map->index, hash, &encoded)) {
        return NULL;
    }

    return (hashmap_str_entry_t *)(uintptr_t)encoded;
}

static bool _hashmap_str_bucket_store(hashmap_str_t *map, u64 hash, hashmap_str_entry_t *head) {
    if (!map || !map->index) {
        return false;
    }

    if (!head) {
        (void)hashmap_remove(map->index, hash);
        return true;
    }

    return hashmap_set(map->index, hash, (u64)(uintptr_t)head);
}

hashmap_str_t *hashmap_str_create(void) {
    return hashmap_str_create_with(NULL, NULL, NULL);
}

hashmap_str_t *hashmap_str_create_with(
    hashmap_str_hash_fn hash_fn,
    hashmap_str_cmp_fn cmp_fn,
    void *private_data
) {
    hashmap_str_t *map = calloc(1, sizeof(*map));
    if (!map) {
        return NULL;
    }

    map->index = hashmap_create();
    if (!map->index) {
        free(map);
        return NULL;
    }

    map->hash_fn = hash_fn ? hash_fn : _hashmap_str_hash_default;
    map->cmp_fn = cmp_fn ? cmp_fn : _hashmap_str_cmp_default;
    map->private_data = private_data;
    return map;
}

void hashmap_str_clear(hashmap_str_t *map) {
    if (!map || !map->index) {
        return;
    }

    for (size_t i = 0; i < map->index->capacity; i++) {
        const hashmap_entry_t *entry = &map->index->entries[i];
        if (entry->state != HASHMAP_SLOT_FULL) {
            continue;
        }

        hashmap_str_entry_t *node = (hashmap_str_entry_t *)(uintptr_t)entry->value;
        while (node) {
            hashmap_str_entry_t *next = node->next;
            free(node);
            node = next;
        }
    }

    hashmap_clear(map->index);
    map->size = 0;
}

void hashmap_str_destroy(hashmap_str_t *map) {
    if (!map) {
        return;
    }

    hashmap_str_clear(map);
    hashmap_destroy(map->index);
    free(map);
}

size_t hashmap_str_size(const hashmap_str_t *map) {
    return map ? map->size : 0;
}

bool hashmap_str_set(hashmap_str_t *map, const char *key, u64 value) {
    if (!map || !map->index || !map->cmp_fn || !map->hash_fn || !key) {
        return false;
    }

    u64 hash = map->hash_fn(key, map->private_data);
    hashmap_str_entry_t *head = _hashmap_str_bucket_head(map, hash);

    for (hashmap_str_entry_t *node = head; node; node = node->next) {
        if (map->cmp_fn(node->key, key, map->private_data)) {
            node->value = value;
            return true;
        }
    }

    hashmap_str_entry_t *node = calloc(1, sizeof(*node));
    if (!node) {
        return false;
    }

    node->key = key;
    node->value = value;
    node->next = head;

    if (!_hashmap_str_bucket_store(map, hash, node)) {
        free(node);
        return false;
    }

    map->size++;
    return true;
}

bool hashmap_str_get(const hashmap_str_t *map, const char *key, u64 *value_out) {
    if (!map || !map->index || !map->cmp_fn || !map->hash_fn || !key) {
        return false;
    }

    u64 hash = map->hash_fn(key, map->private_data);
    hashmap_str_entry_t *node = _hashmap_str_bucket_head(map, hash);
    while (node) {
        if (map->cmp_fn(node->key, key, map->private_data)) {
            if (value_out) {
                *value_out = node->value;
            }
            return true;
        }
        node = node->next;
    }

    return false;
}

bool hashmap_str_remove(hashmap_str_t *map, const char *key) {
    if (!map || !map->index || !map->cmp_fn || !map->hash_fn || !key) {
        return false;
    }

    u64 hash = map->hash_fn(key, map->private_data);
    hashmap_str_entry_t *head = _hashmap_str_bucket_head(map, hash);

    hashmap_str_entry_t *prev = NULL;
    hashmap_str_entry_t *node = head;
    while (node) {
        if (map->cmp_fn(node->key, key, map->private_data)) {
            hashmap_str_entry_t *next = node->next;
            if (!prev && !_hashmap_str_bucket_store(map, hash, next)) {
                return false;
            }
            if (prev) {
                prev->next = next;
            }

            free(node);
            if (map->size) {
                map->size--;
            }
            return true;
        }

        prev = node;
        node = node->next;
    }

    return false;
}
