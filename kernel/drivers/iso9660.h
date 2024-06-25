#pragma once

#include <fs/iso9660.h>

#include "vfs/driver.h"

typedef struct {
    iso_volume_descriptor* pvd;
} iso_device_private;


bool iso_init(vfs_driver* dev, vfs_file_system* fs);
