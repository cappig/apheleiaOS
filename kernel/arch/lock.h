#pragma once

#include <base/types.h>

#define SPINLOCK_UNLOCKED 0
#define SPINLOCK_LOCKED   1

typedef volatile int lock;


void spin_lock(lock* l);
void spin_unlock(lock* l);
