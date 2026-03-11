#pragma once

#include <base/types.h>
#include <stddef.h>

bool tsc_init(void);
u64 tsc_khz(void);
void tsc_spin(size_t ms);
