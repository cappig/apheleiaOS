#pragma once

#include <base/types.h>
#include <stdatomic.h>

typedef volatile atomic_bool lock;


void spin_lock(lock* l);
void spin_unlock(lock* l);
