#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

// Simple first-fit free-list allocator backed by mmap, should probabily be improved in the
// future...

#define ARENA_SIZE     (64U * 1024U)
#define MMAP_THRESHOLD (ARENA_SIZE / 2U)
#define ALIGNMENT      16U
#define PAGE_SIZE      4096U

typedef struct block {
    size_t size; // excludes header
    struct block *next;
    int free;
    int mmap_backed; // 1 if this block owns its own mmap region
} block_t;

#define HEADER_SIZE ((sizeof(block_t) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))


static block_t *free_list = NULL;

static size_t _align(size_t n) {
    return (n + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
}

static size_t _page_align(size_t n) {
    return (n + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);
}

static void *_mmap_pages(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    return p;
}

static void _split(block_t *block, size_t needed) {
    size_t remaining = block->size - needed;
    if (remaining < HEADER_SIZE + ALIGNMENT) {
        return;
    }

    block_t *rest = (block_t *)((char *)block + HEADER_SIZE + needed);
    rest->size = remaining - HEADER_SIZE;
    rest->free = 1;
    rest->mmap_backed = 0;
    rest->next = block->next;

    block->size = needed;
    block->next = rest;
}

// Extend the heap by allocating a new arena
static block_t *_grow_heap(size_t needed) {
    size_t arena = ARENA_SIZE;
    if (needed + HEADER_SIZE > arena) {
        arena = _page_align(needed + HEADER_SIZE);
    }

    void *mem = _mmap_pages(arena);
    if (!mem) {
        return NULL;
    }

    block_t *block = (block_t *)mem;

    block->size = arena - HEADER_SIZE;
    block->free = 1;
    block->mmap_backed = 0;
    block->next = free_list;
    free_list = block;

    return block;
}

void *malloc(size_t size) {
    if (!size) {
        return NULL;
    }

    size = _align(size);

    // Large allocations get their own mmap region
    if (size >= MMAP_THRESHOLD) {
        size_t total = _page_align(HEADER_SIZE + size);
        void *mem = _mmap_pages(total);
        if (!mem) {
            return NULL;
        }

        block_t *block = (block_t *)mem;
        block->size = total - HEADER_SIZE;
        block->free = 0;
        block->mmap_backed = 1;
        block->next = NULL;

        return (char *)block + HEADER_SIZE;
    }

    block_t *curr = free_list;

    while (curr) {
        if (curr->free && curr->size >= size) {
            _split(curr, size);
            curr->free = 0;
            return (char *)curr + HEADER_SIZE;
        }
        curr = curr->next;
    }

    // No suitable block — grow the heap
    block_t *block = _grow_heap(size);
    if (!block) {
        return NULL;
    }

    _split(block, size);
    block->free = 0;
    return (char *)block + HEADER_SIZE;
}

void *calloc(size_t num, size_t size) {
    if (!num || !size) {
        return NULL;
    }

    size_t total = num * size;
    if (size && total / size != num) {
        return NULL;
    }

    void *ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }

    return ptr;
}

void free(void *ptr) {
    if (!ptr) {
        return;
    }

    block_t *block = (block_t *)((char *)ptr - HEADER_SIZE);

    if (block->mmap_backed) {
        size_t total = HEADER_SIZE + block->size;
        munmap(block, total);
        return;
    }

    block->free = 1;

    // Coalesce with next block if it is also free
    if (block->next && block->next->free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }

    if (!size) {
        free(ptr);
        return NULL;
    }

    block_t *block = (block_t *)((char *)ptr - HEADER_SIZE);
    size_t old_size = block->size;

    size = _align(size);

    if (old_size >= size) {
        return ptr;
    }

    void *new_ptr = malloc(size);
    if (!new_ptr) {
        return NULL;
    }

    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}
