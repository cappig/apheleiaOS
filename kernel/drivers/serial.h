#pragma once

#include <base/types.h>
#include <data/ring.h>

#include "vfs/fs.h"

#define SERIAL_DEV_BUFFER_SIZE 256


void init_serial_dev(virtual_fs* vfs);
