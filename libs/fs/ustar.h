#pragma once

#include <base/attributes.h>
#include <base/types.h>

// The Unix Standard TAR (tape archive)
// https://wiki.osdev.org/USTAR
// https://en.wikipedia.org/wiki/Tar_(computing)#UStar_format

#define USTAR_BLOCK_SIZE 512

typedef struct PACKED {
    char name[100];
    char mode[8];

    char owner_id[8];
    char group_id[8];

    char size[12];
    char last_mod_time[12];
    char checksum[8];

    char type;
    char link_name[100];

    // USTAR extention
    char ustar[6];
    char version[2];

    char owner_name[32];
    char group_name[32];

    char dev_major[8];
    char dev_minor[8];

    char prefix[155];

    // This struct has to take 512 bytes
    u8 _padding[12];
} ustar_header;

enum ustart_type {
    USTAR_TYPE_NORMAL = '0',
    USTAR_TYPE_HARD_LINK = '1',
    USTAR_TYPE_SYM_LINK = '2',
    USTAR_TYPE_CHAR_DEV = '3',
    USTAR_TYPE_BLOCK_DEV = '4',
    USTAR_TYPE_DIR = '5',
    USTAR_TYPE_FIFO = '6',
    // Some other flags exist as well but we dont' support them
};


u32 ustar_to_num(char* str, int size);

void* ustar_find(void* addr, usize size, const char* file);

isize ustar_read(ustar_header* head, void* buf, usize offset, usize len);
