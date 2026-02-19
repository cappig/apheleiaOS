#include "stats.h"

#include <string.h>

static volatile u64 stats_timer_irq_ns = 0;
static volatile u64 stats_sched_switch_count = 0;
static volatile u64 stats_poll_sleep_loops = 0;
static volatile u64 stats_ws_fb_write_bytes = 0;
static volatile u64 stats_fb_present_bytes = 0;
static volatile u64 stats_wm_dirty_pixels = 0;

void stats_reset(void) {
    __atomic_store_n(&stats_timer_irq_ns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats_sched_switch_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats_poll_sleep_loops, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats_ws_fb_write_bytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats_fb_present_bytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stats_wm_dirty_pixels, 0, __ATOMIC_RELAXED);
}

void stats_take_snapshot(stats_snapshot_t *out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->timer_irq_ns = __atomic_load_n(&stats_timer_irq_ns, __ATOMIC_RELAXED);
    out->sched_switch_count = __atomic_load_n(&stats_sched_switch_count, __ATOMIC_RELAXED);
    out->poll_sleep_loops = __atomic_load_n(&stats_poll_sleep_loops, __ATOMIC_RELAXED);
    out->ws_fb_write_bytes = __atomic_load_n(&stats_ws_fb_write_bytes, __ATOMIC_RELAXED);
    out->fb_present_bytes = __atomic_load_n(&stats_fb_present_bytes, __ATOMIC_RELAXED);
    out->wm_dirty_pixels = __atomic_load_n(&stats_wm_dirty_pixels, __ATOMIC_RELAXED);
}

void stats_add_timer_irq_ns(u64 ns) {
    __atomic_fetch_add(&stats_timer_irq_ns, ns, __ATOMIC_RELAXED);
}

void stats_inc_sched_switch_count(void) {
    __atomic_fetch_add(&stats_sched_switch_count, 1, __ATOMIC_RELAXED);
}

void stats_inc_poll_sleep_loops(void) {
    __atomic_fetch_add(&stats_poll_sleep_loops, 1, __ATOMIC_RELAXED);
}

void stats_add_ws_fb_write_bytes(u64 bytes) {
    __atomic_fetch_add(&stats_ws_fb_write_bytes, bytes, __ATOMIC_RELAXED);
}

void stats_add_fb_present_bytes(u64 bytes) {
    __atomic_fetch_add(&stats_fb_present_bytes, bytes, __ATOMIC_RELAXED);
}

void stats_add_wm_dirty_pixels(u64 pixels) {
    __atomic_fetch_add(&stats_wm_dirty_pixels, pixels, __ATOMIC_RELAXED);
}
