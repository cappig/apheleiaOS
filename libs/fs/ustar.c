#include "ustar.h"

#include <stdlib.h>
#include <string.h>


// ustar stores values in _ASCII octal_ yeah
u32 ustar_to_num(char* str, int size) {
    u32 ret = 0;
    unsigned char* ch = (unsigned char*)str;

    while (size-- > 0) {
        ret *= 8;
        ret += *ch - '0';
        ch++;
    }

    return ret;
}


// Assuming that the file is loaded in contiguous ram
ustar_file* ustar_find(void* addr, usize size, const char* file) {
    ustar_header* head = addr;
    u8* ptr = addr;

    for (;;) {
        if (strncmp(head->ustar, "ustar", 5))
            return NULL;

        if ((usize)ptr - (usize)addr > size)
            return NULL;

        if (!strcmp(head->name, file))
            return (ustar_file*)head;

        usize file_size = ustar_to_num(head->size, 11);

        ptr += (((file_size + 511) / 512) + 1) * 512;
        head = (ustar_header*)ptr;
    }

    return NULL;
}


isize ustar_read(ustar_header* head, void* buf, usize offset, usize len) {
    if (offset == len)
        return 0;
    if (offset > len)
        return -1;

    usize file_size = ustar_to_num(head->size, 11);
    usize read_size = min(len, file_size);

    void* read_base = head + USTAR_BLOCK_SIZE;

    memcpy(buf, read_base + offset, read_size);

    return read_size - offset;
}
