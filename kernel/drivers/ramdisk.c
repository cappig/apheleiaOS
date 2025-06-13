#include "ramdisk.h"

#include <base/types.h>
#include <stdlib.h>
#include <string.h>

#include "mem/heap.h"
#include "sys/disk.h"


static isize _read(disk_dev* dev, void* dest, usize offset, usize bytes) {
    ramdisk_private* private = dev->private;

    if (offset > private->size)
        return -1;

    bytes = min(bytes, private->size - offset);

    void* src = private->addr + offset;
    memcpy(dest, src, bytes);

    return bytes;
}

static isize _write(disk_dev* dev, void* dest, usize offset, usize bytes) {
    ramdisk_private* private = dev->private;

    // if (!private->write)
    //    return -1;

    if (offset > private->size)
        return -1;

    bytes = min(bytes, private->size - offset);

    void* src = private->addr + offset;
    memcpy(src, dest, bytes);

    return bytes;
}


disk_dev* ramdisk_init(char* name, void* addr, usize size, bool write) {
    disk_dev_interface* interface = kcalloc(sizeof(disk_dev_interface));
    interface->read = _read;
    interface->write = write ? _write : NULL;

    ramdisk_private* private = kcalloc(sizeof(ramdisk_private));
    private->write = write;
    private->size = size;
    private->addr = addr;

    // disk_dev* dev = vfs_create_disk(name, 1, size);
    disk_dev* dev = kcalloc(sizeof(disk_dev));

    dev->name = strdup(name);
    dev->type = DISK_VIRTUAL;

    dev->sector_size = 1;
    dev->sector_count = size;

    dev->interface = interface;

    dev->private = private;

    return dev;
}
