#include "stddef.h"
#include "stdlib.h"
#include "string.h"

#define ELEM_PTR(base, size, i) ((void*)((size_t)base + size * i))

typedef int (*comp_fn)(const void*, const void*);


static size_t _partition(void* base, size_t low, size_t high, size_t size, comp_fn comp) {
    size_t pivot_index = ((high - low) / 2) + low;
    void* pivot = ELEM_PTR(base, size, pivot_index);

    size_t i = low - 1;
    size_t j = high + 1;

    for (;;) {
        do {
            i++;
        } while (comp(ELEM_PTR(base, size, i), pivot) < 0);

        do {
            j--;
        } while (comp(ELEM_PTR(base, size, j), pivot) > 0);

        if (i >= j)
            return j;

        memswap(ELEM_PTR(base, size, i), ELEM_PTR(base, size, j), size);
    }
}

// A quicksort algorithm based on the pseudocode from wikipedia:
// https://en.wikipedia.org/wiki/Quicksort#algorithm (Hoare partition scheme)
static void _quick_sort(void* base, size_t low, size_t high, size_t size, comp_fn comp) {
    if (low < high) {
        size_t pivot = _partition(base, low, high, size, comp);

        _quick_sort(base, low, pivot, size, comp);
        _quick_sort(base, pivot + 1, high, size, comp);
    }
}

void qsort(void* base, size_t num, size_t size, comp_fn comp) {
    _quick_sort(base, 0, num - 1, size, comp);
}
