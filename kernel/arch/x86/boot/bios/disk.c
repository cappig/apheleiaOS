#include "disk.h"

#include <base/macros.h>
#include <base/types.h>
#include <fs/ext2.h>
#include <stdint.h>

#include "bios.h"
#include "tty.h"
#include "x86/lib/regs.h"

static u16 disk_code;
static disk_params_t params;


static void _read_disk_parameters(u16 disk) {
    params.size = sizeof(disk_params_t);

    regs32_t d_regs = {0};
    d_regs.ah = 0x48;
    d_regs.dx = disk;
    d_regs.esi = (u32)(uintptr_t)&params;

    bios_call(0x13, &d_regs, &d_regs);

    if (d_regs.flags & FLAG_CF)
        panic("Error reading disk parameters!");
}


int read_disk(void* dest, size_t offset, size_t bytes) {
    dap_t dap = {
        .size = sizeof(dap_t),
        .sectors = DIV_ROUND_UP(bytes, params.bytes_per_sector),
        .destination = (u32)(uintptr_t)dest,
        .lba = DIV_ROUND_UP(offset, params.bytes_per_sector),
    };

    regs32_t d_regs = {0};
    d_regs.ah = 0x42;
    d_regs.dx = disk_code;
    d_regs.esi = (u32)(uintptr_t)&dap;

    bios_call(0x13, &d_regs, &d_regs);

    if (d_regs.flags & FLAG_CF)
        panic("Error reading disk!");

    return bytes;
}

void ext2_init(void) {
    static ext2_superblock_t superblock;

    // Read the superblock at offset 1024
    read_disk(&superblock, 1024, sizeof(ext2_superblock_t));

    if (superblock.signature != EXT2_SIGNATURE)
        panic("Not an EXT2 filesystem!");
}
