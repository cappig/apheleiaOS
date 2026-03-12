#pragma once

#include "scheduler.h"

bool sched_user_region_mark_cow(
    sched_thread_t *thread,
    sched_user_region_t *region
);
