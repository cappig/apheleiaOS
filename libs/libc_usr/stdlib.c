#include <aos/syscalls.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define ALIGNMENT        sizeof(void*)
#define ALIGN_SIZE(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

#define MMAP_CHUNK_SIZE (16 * 1024)
#define MIN_BLOCK_SIZE  ALIGN_SIZE(sizeof(struct block_header))

typedef struct block_header {
    size_t size;
    bool is_free;

    struct block_header* next;
    struct block_header* prev;
} block_header;


static block_header* free_list_head = NULL;


static inline block_header* _get_header(void* ptr) {
    if (!ptr)
        return NULL;

    return (block_header*)((char*)ptr - sizeof(block_header));
}

static void _add_to_free_list(block_header* block) {
    block_header* current = free_list_head;
    block_header* prev = NULL;

    while (current && current < block) {
        prev = current;
        current = current->next;
    }

    block->prev = prev;
    block->next = current;

    if (prev)
        prev->next = block;
    else
        free_list_head = block;

    if (current)
        current->prev = block;
}

static void _remove_from_free_list(block_header* block) {
    if (block->prev)
        block->prev->next = block->next;
    else
        free_list_head = block->next;

    if (block->next)
        block->next->prev = block->prev;

    block->next = NULL;
    block->prev = NULL;
}

static block_header* _find_free_block(size_t size) {
    block_header* current = free_list_head;

    while (current) {
        if (current->is_free && current->size >= size)
            return current;

        current = current->next;
    }

    return NULL;
}

static block_header* _get_memory(size_t size) {
    size_t total_size = ALIGN_SIZE(size + sizeof(block_header));

    if (total_size < MMAP_CHUNK_SIZE)
        total_size = MMAP_CHUNK_SIZE;

    void* ptr =
        sys_mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (!ptr)
        return NULL;

    block_header* block = (block_header*)ptr;

    block->size = total_size - sizeof(block_header);
    block->is_free = true;
    block->next = NULL;
    block->prev = NULL;

    _add_to_free_list(block);

    return block;
}

static void _split_block(block_header* block, size_t size) {
    if (block->size < size + sizeof(block_header) + MIN_BLOCK_SIZE)
        return;

    block_header* new_block = (block_header*)((void*)block + sizeof(block_header) + size);

    new_block->size = block->size - size - sizeof(block_header);
    new_block->is_free = true;
    new_block->prev = NULL;
    new_block->next = NULL;

    block->size = size;

    _add_to_free_list(new_block);
}

static void _coalesce_block(block_header* block) {
    block_header* next = block->next;
    if (next && (char*)block + sizeof(block_header) + block->size == (char*)next) {
        block->size += sizeof(block_header) + next->size;
        _remove_from_free_list(next);
    }

    block_header* prev = block->prev;
    if (prev && (char*)prev + sizeof(block_header) + prev->size == (char*)block) {
        prev->size += sizeof(block_header) + block->size;
        _remove_from_free_list(block);
    }
}


void* malloc(size_t size) {
    if (!size)
        return NULL;

    size = ALIGN_SIZE(size);

    block_header* block = _find_free_block(size);

    if (!block) {
        block = _get_memory(size);

        if (!block)
            return NULL;
    }

    _remove_from_free_list(block);
    _split_block(block, size);

    block->is_free = false;

    return (void*)((char*)block + sizeof(block_header));
}

void free(void* ptr) {
    if (!ptr)
        return;

    block_header* block = _get_header(ptr);

    if (block->is_free)
        return;

    block->is_free = true;

    _add_to_free_list(block);
    _coalesce_block(block);
}

void* calloc(size_t num, size_t size) {
    if (!num || !size)
        return NULL;

    if (SIZE_MAX / num < size)
        return NULL;

    size_t total = num * size;
    void* ptr = malloc(total);

    if (ptr)
        memset(ptr, 0, total);

    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr)
        return malloc(size);

    if (!size) {
        free(ptr);
        return NULL;
    }

    block_header* old_block = _get_header(ptr);

    size_t old_size = old_block->size;
    size_t new_size = ALIGN_SIZE(size);

    if (new_size <= old_size)
        return ptr;

    void* new_ptr = malloc(new_size);

    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, old_size);
    free(ptr);

    return new_ptr;
}
