#pragma once

#include <base/types.h>
#include <data/ring.h>

#define SERIAL_DEV_BUFFER_SIZE 256


bool init_serial_port(u8 index);

void init_serial_dev(void);
