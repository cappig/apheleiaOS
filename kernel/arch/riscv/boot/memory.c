#include <base/types.h>

void *malloc(size_t size) {
    (void)size;
    return 0;
}

void *calloc(size_t count, size_t size) {
    (void)count;
    (void)size;
    return 0;
}

void *realloc(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
    return 0;
}

void free(void *ptr) {
    (void)ptr;
}
