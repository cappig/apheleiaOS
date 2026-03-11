#include "scheduler_internal.h"

void sched_wake_sleepers_locked(u64 now) {
    for (;;) {
        sched_thread_t *thread = sleep_heap_top();
        if (!thread) {
            break;
        }

        if (!thread->sleep_queued || !thread->wake_tick) {
            sleep_heap_remove_at(0);
            continue;
        }

        if (thread->wake_tick > now) {
            break;
        }

        sleep_heap_remove(thread);
        bool had_deadline = thread->wait_deadline_tick != 0;
        thread->wake_tick = 0;
        thread->wait_deadline_tick = 0;

        thread_state_t state = sched_thread_state_load(thread);
        if (state == THREAD_SLEEPING) {
            if (thread->in_wait_queue) {
                wait_queue_remove_locked(thread);
            }

            thread->wait_result = (u8)SCHED_WAIT_TIMEOUT;
            __atomic_fetch_add(&sched_state.metrics.wait_timeout_count, 1, __ATOMIC_RELAXED);

            if (sched_reclaim_handoff_thread_locked(thread)) {
                sched_thread_state_store(thread, THREAD_READY);
                enqueue_thread_with_ipi(thread, true);
                continue;
            }

            if (sched_thread_owned_by_current_cpu(thread)) {
                // a timeout raced with the still current task on this CPU.. let
                // the owner CPU resume the task in place
                sched_nudge_owned_thread_locked(thread);
                continue;
            }

            sched_thread_mark_not_running(thread);
            sched_thread_state_store(thread, THREAD_READY);

            // Timeout wakeups must actively nudge the target CPU; otherwise a
            // remote busy core can leave a timer-woken task runnable but not
            // scheduled for far too long.
            enqueue_thread_with_ipi(thread, true);

            continue;
        }

        if (state == THREAD_RUNNING) {
            (void)sched_repair_unowned_running_thread_locked(
                thread,
                "sched_wake_sleepers",
                true
            );
            continue;
        }

        (void)had_deadline;
    }
}

void sched_wake_sleepers(u64 now) {
    unsigned long sched_flags = 0;
    if (!sched_lock_try_save(&sched_flags)) {
        return;
    }

    sched_wake_sleepers_locked(now);
    sched_lock_restore(sched_flags);
}

static bool wait_queue_unlink_thread_locked(
    sched_wait_queue_t *queue,
    sched_thread_t *thread
) {
    if (!queue || !queue->list || !thread) {
        return false;
    }

    list_node_t *target = &thread->wait_node;
    if (target->owner == queue->list) {
        list_node_t *prev = target->prev;
        list_node_t *next = target->next;
        bool linked_consistent = (
            (!prev || prev->next == target) &&
            (!next || next->prev == target) &&
            (target == queue->list->head || prev) &&
            (target == queue->list->tail || next)
        );

        if (linked_consistent) {
            if (prev) {
                prev->next = next;
            } else {
                queue->list->head = next;
            }

            if (next) {
                next->prev = prev;
            } else {
                queue->list->tail = prev;
            }

            if (queue->list->length) {
                queue->list->length--;
            }
            if (queue->waiter_count) {
                queue->waiter_count--;
            }

            target->next = NULL;
            target->prev = NULL;
            target->owner = NULL;
            return true;
        }
    }

    list_node_t *prev = NULL;
    size_t limit = queue->list->length;
    if (queue->waiter_count > limit) {
        limit = queue->waiter_count;
    }
    if (!limit) {
        limit = 1;
    }

    for (list_node_t *it = queue->list->head; it && limit--; it = it->next) {
        if (it != &thread->wait_node && it->data != thread) {
            prev = it;
            continue;
        }

        list_node_t *next = it->next;
        if (prev) {
            prev->next = next;
        } else {
            queue->list->head = next;
        }

        if (next) {
            next->prev = prev;
        } else {
            queue->list->tail = prev;
        }

        if (queue->list->length) {
            queue->list->length--;
        }
        if (queue->waiter_count) {
            queue->waiter_count--;
        }

        it->next = NULL;
        it->prev = NULL;
        it->owner = NULL;
        return true;
    }

    return false;
}

void wait_queue_remove_locked(sched_thread_t *thread) {
    if (!thread || !thread->in_wait_queue || !thread->blocked_on) {
        return;
    }

    if (!thread->blocked_on->list) {
        thread->in_wait_queue = false;
        thread->blocked_on = NULL;
        return;
    }

    if (thread->in_wait_queue && thread->blocked_on && thread->blocked_on->list) {
        sched_wait_queue_t *queue = thread->blocked_on;
        (void)wait_queue_unlink_thread_locked(queue, thread);

        thread->wait_node.next = NULL;
        thread->wait_node.prev = NULL;
        thread->wait_node.owner = NULL;
        thread->in_wait_queue = false;
        thread->blocked_on = NULL;
    }
}

void wait_queue_remove(sched_thread_t *thread) {
    if (!thread || !thread->in_wait_queue || !thread->blocked_on) {
        return;
    }

    unsigned long flags = sched_lock_save();
    wait_queue_remove_locked(thread);
    sched_lock_restore(flags);
}

bool sched_wait_until_running(sched_thread_t *self) {
    if (!self) {
        return false;
    }

    bool resched_kicked = false;

    for (;;) {
        thread_state_t state = sched_thread_state_load(self);
        if (state == THREAD_RUNNING) {
            if (sched_local_current() == self) {
                return true;
            }
        }

        if (!sched_is_running()) {
            return false;
        }

        if (state == THREAD_ZOMBIE) {
            return false;
        }

        if (!resched_kicked) {
            sched_request_resched_local_force();
            resched_kicked = true;
        }
        sched_spin_wait();
    }
}

void sched_discard_thread(sched_thread_t *thread) {
    if (!thread || sched_thread_is_idle(thread)) {
        return;
    }

    run_queue_remove(thread);
    wait_queue_remove(thread);
    sleep_heap_remove(thread);
    sched_thread_mark_not_running(thread);

    if (thread->in_zombie_list && sched_state.zombie_list) {
        unsigned long flags = sched_lock_save();

        if (thread->in_zombie_list) {
            list_remove(sched_state.zombie_list, &thread->zombie_node);
            thread->in_zombie_list = false;
        }

        sched_lock_restore(flags);
    }

    remove_all_thread(thread);
    sched_thread_put(thread);
}

void sched_make_runnable(sched_thread_t *thread) {
    if (!thread || sched_thread_is_idle(thread)) {
        return;
    }

    unsigned long flags = sched_lock_save();

    wait_queue_remove_locked(thread);
    sleep_heap_remove(thread);
    thread->wake_tick = 0;
    thread->wait_deadline_tick = 0;
    thread->wait_result = (u8)SCHED_WAIT_WOKEN;

    if (sched_reclaim_handoff_thread_locked(thread)) {
        sched_thread_state_store(thread, THREAD_READY);
        enqueue_thread(thread);
        sched_lock_restore(flags);
        return;
    }

    if (sched_thread_owned_by_current_cpu(thread)) {
        sched_nudge_owned_thread_locked(thread);
        sched_lock_restore(flags);
        return;
    }

    if (sched_thread_state_load(thread) == THREAD_RUNNING) {
        (void)sched_repair_unowned_running_thread_locked(
            thread,
            "sched_make_runnable",
            true
        );
        sched_lock_restore(flags);
        return;
    }

    sched_thread_mark_not_running(thread);
    sched_thread_state_store(thread, THREAD_READY);
    enqueue_thread(thread);

    sched_lock_restore(flags);
}

void sched_unblock_thread(sched_thread_t *thread) {
    if (!thread || sched_thread_is_idle(thread)) {
        return;
    }

    unsigned long flags = sched_lock_save();

    wait_queue_remove_locked(thread);
    sleep_heap_remove(thread);

    thread_state_t state = sched_thread_state_load(thread);
    if (state == THREAD_SLEEPING || state == THREAD_READY) {
        thread->wake_tick = 0;
        thread->wait_deadline_tick = 0;

        thread->wait_result = (u8)(
            ((thread->wait_flags & SCHED_WAIT_INTERRUPTIBLE) &&
             sched_signal_has_pending(thread))
                ? SCHED_WAIT_INTR
                : SCHED_WAIT_WOKEN
        );

        if (sched_reclaim_handoff_thread_locked(thread)) {
            sched_thread_state_store(thread, THREAD_READY);
            enqueue_thread(thread);
            sched_lock_restore(flags);
            return;
        }

        if (sched_thread_owned_by_current_cpu(thread)) {
            sched_nudge_owned_thread_locked(thread);
            sched_lock_restore(flags);
            return;
        }

        sched_thread_mark_not_running(thread);
        sched_thread_state_store(thread, THREAD_READY);
        enqueue_thread(thread);
    } else if (state == THREAD_RUNNING) {
        (void)sched_repair_unowned_running_thread_locked(
            thread,
            "sched_unblock_thread",
            true
        );
    }

    sched_lock_restore(flags);
}

void sched_stop_thread(sched_thread_t *thread, int signum) {
    if (!thread || sched_thread_is_idle(thread)) {
        return;
    }

    unsigned long flags = sched_lock_save();
    thread_state_t state = sched_thread_state_load(thread);
    if (state == THREAD_ZOMBIE || state == THREAD_STOPPED) {
        sched_lock_restore(flags);
        return;
    }

    bool request_local_resched = false;
    size_t request_remote_resched_cpu = MAX_CORES;
    bool stopping_current = (thread == sched_local_current());

    if (
        !stopping_current &&
        state == THREAD_RUNNING &&
        sched_thread_running_cpu_load(thread) >= 0 &&
        sched_thread_owned_by_running_cpu(thread)
    ) {
        if (signum > 0 && signum < NSIG) {
            u32 mask = 1u << (signum - 1);
            __atomic_fetch_or(&thread->signal_pending, mask, __ATOMIC_ACQ_REL);
        }

        thread->stop_signal = signum;
        thread->stop_reported = false;

        size_t target_cpu = (size_t)sched_thread_running_cpu_load(thread);
        if (target_cpu == sched_cpu_id()) {
            request_local_resched = true;
        } else if (target_cpu < MAX_CORES) {
            request_remote_resched_cpu = target_cpu;
        }

        sched_lock_restore(flags);
        if (request_local_resched) {
            sched_request_resched_local();
        } else if (
            request_remote_resched_cpu < MAX_CORES &&
            sched_send_wake_ipi(request_remote_resched_cpu)
        ) {
            __atomic_fetch_add(&sched_state.metrics.wake_ipi, 1, __ATOMIC_RELAXED);
        }
        return;
    }

    run_queue_remove(thread);
    wait_queue_remove_locked(thread);
    sleep_heap_remove(thread);

    thread->wake_tick = 0;
    sched_thread_state_store(thread, THREAD_STOPPED);
    thread->stop_signal = signum;
    thread->stop_reported = false;

    if (!stopping_current) {
        sched_thread_mark_not_running(thread);
    }

    if (thread->user_thread) {
        sched_thread_t *parent = find_thread_by_pid_locked(thread->ppid);

        if (parent) {
            sched_wake_one(&parent->wait_queue);
            (void)sched_signal_send_thread(parent, SIGCHLD);
        }
    }

    sched_lock_restore(flags);

    if (stopping_current && sched_running_get()) {
        sched_yield();
    }
}

void sched_continue_thread(sched_thread_t *thread) {
    if (!thread || sched_thread_is_idle(thread)) {
        return;
    }

    unsigned long flags = sched_lock_save();
    if (sched_thread_state_load(thread) != THREAD_STOPPED) {
        sched_lock_restore(flags);
        return;
    }

    sched_thread_mark_not_running(thread);
    sched_thread_state_store(thread, THREAD_READY);
    thread->stop_signal = 0;
    thread->stop_reported = false;
    enqueue_thread(thread);

    sched_lock_restore(flags);
}
