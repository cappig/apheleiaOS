#pragma once

#include <base/types.h>

typedef struct {
    u64 timer_irq_ns;
    u64 sched_switch_count;
    u64 poll_sleep_loops;
    u64 ws_fb_write_bytes;
    u64 fb_present_bytes;
    u64 wm_dirty_pixels;
} stats_snapshot_t;

void stats_reset(void);
void stats_take_snapshot(stats_snapshot_t *out);

void stats_add_timer_irq_ns(u64 ns);
void stats_inc_sched_switch_count(void);
void stats_inc_poll_sleep_loops(void);
void stats_add_ws_fb_write_bytes(u64 bytes);
void stats_add_fb_present_bytes(u64 bytes);
void stats_add_wm_dirty_pixels(u64 pixels);
