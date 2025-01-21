#include "ramdisk.h"

#include <base/types.h>
#include <string.h>

#include "mem/heap.h"
#include "vfs/driver.h"


static isize _read(vfs_driver* dev, void* dest, usize offset, usize bytes) {
    ramdisk_private* private = dev->private;

    if (offset + bytes > private->size)
        return 0;

    void* src = private->addr + offset;
    memcpy(dest, src, bytes);

    return bytes;
}

static isize _write(vfs_driver* dev, void* dest, usize offset, usize bytes) {
    ramdisk_private* private = dev->private;

    // if (!private->write)
    //    return -1;

    if (offset + bytes > private->size)
        return 0;

    void* src = private->addr + offset;
    memcpy(src, dest, bytes);

    return bytes;
}


vfs_driver* ramdisk_init(char* name, void* addr, usize size, bool write) {
    vfs_drive_interface* interface = kcalloc(sizeof(vfs_drive_interface));
    interface->read = _read;
    interface->write = write ? _write : NULL;

    ramdisk_private* private = kcalloc(sizeof(ramdisk_private));
    private->write = write;
    private->size = size;
    private->addr = addr;

    vfs_driver* dev = vfs_create_device(name, 1, size);

    dev->type = VFS_DRIVER_VIRTUAL;
    dev->private = private;
    dev->interface = interface;

    return dev;
}
