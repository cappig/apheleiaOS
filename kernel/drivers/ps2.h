#pragma once

#include <base/types.h>
#include <data/ring.h>

#define PS2_RELEASE_OFFSET 0x80
#define PS2_RELEASED(key)  (key + PS2_RELEASE_OFFSET)

#define PS2_EXTENDED 0xE0


void init_ps2_kbd(void);
