#include "disk.h"

#include <base/macros.h>
#include <base/types.h>
#include <fs/ext2.h>

#include "bios.h"
#include "tty.h"

static u16 disk_code;
static disk_parameters params;


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
        .destination = (u32)(uptr)dest,
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

void ext2_init(void) {
    static ext2_superblock sb;

    // Read the superblock at offset 1024
    read_disk(&sb, 1024, sizeof(ext2_superblock));

    if (sb.signature != EXT2_SIGNATURE)
        panic("Not an EXT2 filesystem!");
}
