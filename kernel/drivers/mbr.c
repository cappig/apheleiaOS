#include "mbr.h"

#include <base/macros.h>
#include <boot/mbr.h>
#include <log/log.h>
#include <string.h>

#include "mem/heap.h"
#include "sys/disk.h"
#include "sys/panic.h"

char* mbr_type_string(enum mbr_partition_type type) {
    switch (type) {
    case MBR_EMPTY:
        return "empty";
    case MBR_FAT12:
        return "FAT 12";
    case MBR_FAT16_SMALL:
        return "FAT 16 (small)";
    case MBR_FAT16_LARGE:
        return "FAT 16 (large)";
    case MBR_NTFS:
        return "NTFS";
    case MBR_FAT32_CHS:
        return "FAT 32 (CHS)";
    case MBR_FAT32_LBA:
        return "FAT 32 (LBA)";
    case MBR_FAT16_LBA:
        return "FAT 16 (LBA)";
    case MBR_LINUX:
        return "linux native";
    case MBR_LINUX_SWAP:
        return "linux swap";
    case MBR_ISO:
        return "ISO 9660";
    case MBR_GPT:
        return "protective GPT";
    default:
        return "unknown";
    }
}


bool mbr_is_empty(mbr_table* table) {
    for (usize i = 0; i < 4; i++)
        if (table->partitions[i].type)
            return false;

    return true;
}

void dump_mbr(mbr_table* table) {
    bool printed = false;

    for (usize i = 0; i < 4; i++) {
        if (!table->partitions[i].type)
            continue;

        log_debug(
            "[ start: %d | size: %dKiB | type: %#.2x (%s) ]",
            table->partitions[i].lba_first,
            (table->partitions[i].sector_count * 512) / KiB,
            table->partitions[i].type,
            mbr_type_string(table->partitions[i].type)
        );

        printed = true;
    }

    if (!printed)
        log_debug("[ empty table ]");
}

mbr_table* parse_mbr(disk_dev* dev) {
    master_boot_record* mbr = kmalloc(sizeof(master_boot_record));
    dev->interface->read(dev, mbr, 0, 512);

    if (mbr->signature != MBR_SIGNATURE) {
        kfree(mbr);
        return NULL;
    }

    // TODO: perform more checks
    mbr_table* table = kmalloc(sizeof(mbr_table));
    assert(table);

    memcpy(table, &mbr->table, 4 * sizeof(mbr_partition));

    kfree(mbr);

    return table;
}
