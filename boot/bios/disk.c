#include "disk.h"

#include <base/addr.h>
#include <base/macros.h>
#include <base/types.h>
#include <boot/eltorito.h>
#include <fs/iso9660.h>
#include <string.h>
#include <x86/serial.h>

#include "bios.h"
#include "memory.h"
#include "tty.h"

static u16 disk_code;
static disk_parameters params;
static iso_volume_descriptor* pvd;


static void _read_disk_parameters(u16 disk) {
    params.size = sizeof(disk_parameters);

    regs d_regs = {0};
    d_regs.ah = 0x48;
    d_regs.dx = disk;
    d_regs.esi = (u32)(uptr)&params;

    bios_call(0x13, &d_regs, &d_regs);

    if (d_regs.flags & FLAG_CF)
        panic("Error reading disk parameters!");
}

isize read_disk(void* dest, usize offset, usize bytes) {
    disk_address_packet dap = {
        .size = sizeof(disk_address_packet),
        .sectors = DIV_ROUND_UP(bytes, params.bytes_per_sector),
        .destination.off = OFFSET(dest),
        .destination.seg = SEGMENT(dest),
        .lba = DIV_ROUND_UP(offset, params.bytes_per_sector),
    };

    regs d_regs = {0};
    d_regs.ah = 0x42;
    d_regs.dx = disk_code;
    d_regs.esi = (u32)(uptr)&dap;

    bios_call(0x13, &d_regs, &d_regs);

    if (d_regs.flags & FLAG_CF)
        panic("Error reading disk!");

    return bytes;
}

void init_disk(u16 disk) {
    disk_code = disk;
    _read_disk_parameters(disk);

    // Locate the PVD
    pvd = bmalloc(sizeof(iso_volume_descriptor), false);

    for (u32 lba = ISO_VOLUME_START; lba < ISO_MAX_VOLUMES; lba++) {
        read_disk(pvd, lba * ISO_SECTOR_SIZE, ISO_SECTOR_SIZE);

        if (pvd->type == ISO_PRIMARY)
            return;

        if (pvd->type == ISO_TERMINATOR)
            break;
    }

    panic("No primary volume found on disk!");
}

// All of the bootloader files are in the root of the iso so we don't even bother with subdirs here
// Keep things simple in the bootloader :^)
void open_root_file(file_handle* file, const char* name) {
    iso_dir* root = (iso_dir*)pvd->root;

    void* buffer = bmalloc(ISO_SECTOR_SIZE, false);
    read_disk(buffer, root->extent_location.lsb * ISO_SECTOR_SIZE, ISO_SECTOR_SIZE);

    usize offset = 0;
    while (offset <= ISO_SECTOR_SIZE) {
        iso_dir* record = (iso_dir*)(buffer + offset);

        // File found, load all the blocks into memory
        if (!strncasecmp(record->file_id, name, strlen(name))) {
            u32 load_size = ALIGN(record->extent_size.lsb, ISO_SECTOR_SIZE);

            // fuck me the disk buffer must be in low memory.
            // We only have some 800K of that so we read to a low
            // buffer sector by sector and then memcpy to a higher addr
            void* low_buffer = bmalloc(ISO_SECTOR_SIZE, false);
            void* file_buffer = bmalloc(load_size, true);

            for (u32 i = 0; i < load_size / ISO_SECTOR_SIZE; i++) {
                u32 addr = (record->extent_location.lsb + i) * ISO_SECTOR_SIZE;
                read_disk(low_buffer, addr, ISO_SECTOR_SIZE);

                memcpy(file_buffer + i * ISO_SECTOR_SIZE, low_buffer, ISO_SECTOR_SIZE);
            }

            bfree(low_buffer);
            bfree(buffer);

            file->addr = file_buffer;
            file->size = record->extent_size.lsb;
            return;
        }

        if (!record->length)
            break;

        offset += record->length;
    }

    bfree(buffer);

    // We didn't find the file
    file->addr = NULL;
    file->size = 0;
}

// We don't _really_ need this because all E820_ALLOC regions get reclaimed by the kernel
void close_root_file(file_handle* file) {
    if (!file->size)
        return;

    bfree(file->addr);
    file->size = 0;
}
