#include "internal.h"

static void wait_queue_free_container(linked_list_t *list) {
    if (!list) {
        return;
    }

    list->head = NULL;
    list->tail = NULL;
    list->length = 0;
    free(list);
}

static void wake_waiter(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    sleep_heap_remove(thread);
    thread->wake_tick = 0;
    thread->wait_deadline_tick = 0;
    thread->wait_result = (u8)SCHED_WAIT_WOKEN;

    if (sched_reclaim_handoff(thread)) {
        thread_state_t state = thread_get_state(thread);

        if (state == THREAD_ZOMBIE || state == THREAD_STOPPED) {
            return;
        }

        thread_set_state(thread, THREAD_READY);
        enqueue_thread(thread);

        return;
    }

    if (thread_in_handoff(thread)) {
        thread_state_t state = thread_get_state(thread);

        if (state == THREAD_ZOMBIE || state == THREAD_STOPPED) {
            return;
        }

        thread_set_state(thread, THREAD_READY);
        sched_nudge_thread(thread);
        return;
    }

    if (thread_on_local_cpu(thread)) {
        sched_nudge_thread(thread);
        return;
    }

    thread_state_t state = thread_get_state(thread);

    if (state == THREAD_RUNNING) {
        sched_repair_thread(thread, true);
        return;
    }

    if (state != THREAD_SLEEPING && state != THREAD_READY) {
        return;
    }

    thread_set_cpu(thread, -1);
    thread_set_state(thread, THREAD_READY);

    enqueue_thread(thread);
}

static sched_thread_t *wait_queue_pop(sched_wait_queue_t *queue) {
    if (!queue || !queue->list) {
        return NULL;
    }

    for (;;) {
        list_node_t *node = queue->list->head;
        if (node) {
            list_node_t *next = node->next;

            if (next) {
                next->prev = NULL;
            } else {
                queue->list->tail = NULL;
            }

            queue->list->head = next;

            node->next = NULL;
            node->prev = NULL;
            node->owner = NULL;

            if (queue->list->length) {
                queue->list->length--;
            }
            if (queue->waiter_count) {
                queue->waiter_count--;
            }
        }

        if (!node) {
            return NULL;
        }

        sched_thread_t *thread = node->data;
        if (!thread) {
            continue;
        }

        if (!thread->in_wait_queue || thread->blocked_on != queue) {
            // drop stale waitnode entries from older wait cycles
            continue;
        }

        thread->in_wait_queue = false;
        thread->blocked_on = NULL;

        return thread;
    }
}

static void wake_queue_one(sched_wait_queue_t *queue) {
    sched_thread_t *thread = wait_queue_pop(queue);
    if (!thread) {
        return;
    }

    wake_waiter(thread);
}

static void wake_queue_all(sched_wait_queue_t *queue) {
    for (;;) {
        sched_thread_t *thread = wait_queue_pop(queue);
        if (!thread) {
            return;
        }

        wake_waiter(thread);
    }
}

static bool sched_queue_has_waiters(const sched_wait_queue_t *queue) {
    if (!queue || !queue->list) {
        return false;
    }

    return __atomic_load_n(&queue->waiter_count, __ATOMIC_ACQUIRE) != 0;
}

void sched_wait_queue_init(sched_wait_queue_t *queue) {
    if (!queue) {
        return;
    }

    spinlock_init(&queue->lock);
    queue->wake_seq = 0;
    queue->waiter_count = 0;
    queue->poll_link = false;

    if (!queue->list) {
        queue->list = list_create();
    }
}

void sched_wait_queue_set_poll_link(sched_wait_queue_t *queue, bool enabled) {
    if (!queue) {
        return;
    }

    queue->poll_link = enabled;
}

void sched_wait_queue_destroy(sched_wait_queue_t *queue) {
    if (!queue) {
        return;
    }

    unsigned long flags = sched_lock_save();

    if (queue->list) {
        __atomic_add_fetch(&queue->wake_seq, 1, __ATOMIC_RELEASE);
        wake_queue_all(queue);
        wait_queue_free_container(queue->list);
        queue->list = NULL;
    }

    queue->waiter_count = 0;
    queue->wake_seq = 0;
    queue->poll_link = false;

    sched_lock_restore(flags);
}

u32 sched_wait_seq(sched_wait_queue_t *queue) {
    if (!queue || !queue->list) {
        return 0;
    }

    return __atomic_load_n(&queue->wake_seq, __ATOMIC_ACQUIRE);
}

sched_wait_result_t sched_wait_on_queue(
    sched_wait_queue_t *queue,
    u32 observed_seq,
    u64 deadline_tick,
    sched_wait_flags_t flags
) {
    sched_thread_t *self = sched_local_current();
    sched_reconcile_lock();

    if (!sched_running_get() || !queue || !queue->list || !self) {
        return SCHED_WAIT_ABORTED;
    }

    if ((flags & SCHED_WAIT_INTERRUPTIBLE) && sched_signal_has_pending(self)) {
        return SCHED_WAIT_INTR;
    }

    sched_cpu_state_t *local = sched_local();

    if (
        sched_preempt_disabled() ||
        lock_spin_held_on_cpu() ||
        local->sched_lock_depth
    ) {
        return SCHED_WAIT_ABORTED;
    }

    unsigned long sched_flags = sched_lock_save();
    if (!queue->list) {
        sched_lock_restore(sched_flags);
        return SCHED_WAIT_ABORTED;
    }

    if ((flags & SCHED_WAIT_INTERRUPTIBLE) && sched_signal_has_pending(self)) {
        sched_lock_restore(sched_flags);
        return SCHED_WAIT_INTR;
    }

    if (
        thread_get_state(self) != THREAD_RUNNING ||
        self != sched_local_current()
    ) {
        sched_lock_restore(sched_flags);
        return SCHED_WAIT_ABORTED;
    }

    if (self->in_wait_queue && self->blocked_on == queue) {
        wq_dequeue(self);
    } else if (self->in_wait_queue || self->blocked_on) {
        wq_dequeue(self);
    }

    rq_remove(self);
    sleep_heap_remove(self);
    self->wake_tick = 0;

    bool unchanged =
        thread_get_state(self) != THREAD_ZOMBIE &&
        __atomic_load_n(&queue->wake_seq, __ATOMIC_ACQUIRE) == observed_seq;

    if (unchanged) {
        u8 next_cookie = (u8)(self->wait_cookie + 1U);
        if (!next_cookie) {
            next_cookie = 1U;
        }

        self->wait_cookie = next_cookie;
        self->wait_flags = flags;
        self->wait_deadline_tick = deadline_tick;
        self->wait_result = (u8)SCHED_WAIT_ABORTED;
        thread_set_state(self, THREAD_SLEEPING);
        self->wait_node.data = self;

        bool appended = list_append(queue->list, &self->wait_node);

        if (!appended) {
            if (list_remove(queue->list, &self->wait_node) && queue->waiter_count) {
                queue->waiter_count--;
            }
            appended = list_append(queue->list, &self->wait_node);
        }

        if (!appended) {
            self->wait_node.next = NULL;
            self->wait_node.prev = NULL;
            self->wait_node.owner = NULL;
            self->wait_flags = 0;
            self->wait_deadline_tick = 0;
            thread_set_state(self, THREAD_RUNNING);
            unchanged = false;
        } else {
            queue->waiter_count++;
            self->in_wait_queue = true;
            self->blocked_on = queue;
        }

        if (unchanged && deadline_tick) {
            self->wake_tick = deadline_tick;

            if (!sleep_heap_insert(self)) {
                if (list_remove(queue->list, &self->wait_node) && queue->waiter_count) {
                    queue->waiter_count--;
                }

                self->wait_node.next = NULL;
                self->wait_node.prev = NULL;
                self->wait_node.owner = NULL;
                self->in_wait_queue = false;
                self->blocked_on = NULL;
                thread_set_state(self, THREAD_RUNNING);
                self->wake_tick = 0;
                self->wait_deadline_tick = 0;
                self->wait_flags = 0;
                unchanged = false;
            }
        }
    }
    sched_lock_restore(sched_flags);

    if (!unchanged) {
        return SCHED_WAIT_ABORTED;
    }

    force_resched();
    if (!wait_running(self)) {
        return SCHED_WAIT_ABORTED;
    }

    sched_wait_result_t result =
        (sched_wait_result_t)__atomic_load_n(&self->wait_result, __ATOMIC_ACQUIRE);

    if (result == SCHED_WAIT_ABORTED) {
        if ((flags & SCHED_WAIT_INTERRUPTIBLE) && sched_signal_has_pending(self)) {
            result = SCHED_WAIT_INTR;
        } else if (deadline_tick && arch_timer_ticks() >= deadline_tick) {
            result = SCHED_WAIT_TIMEOUT;
        }
    }

    self->wait_deadline_tick = 0;
    self->wait_flags = 0;
    self->wait_result = (u8)SCHED_WAIT_ABORTED;

    return result;
}

bool sched_block_if_unchanged(sched_wait_queue_t *queue, u32 observed_seq) {
    return sched_wait_on_queue(queue, observed_seq, 0, 0) == SCHED_WAIT_WOKEN;
}

void sched_block(sched_wait_queue_t *queue) {
    u32 seq = sched_wait_seq(queue);
    sched_block_if_unchanged(queue, seq);
}

void exit_event_push(pid_t pid) {
    if (pid <= 0) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&sched_state.exit_events.lock);

    ring_queue_t *r = sched_state.exit_events.ring;
    if (r) {
        if (ring_queue_count(r) >= ring_queue_capacity(r)) {
            ring_queue_drop_head(r);
        }

        ring_queue_push(r, &pid);
    }

    spin_unlock_irqrestore(&sched_state.exit_events.lock, flags);

    if (sched_running_get() && sched_state.exit_event_wait.list) {
        sched_wake_all(&sched_state.exit_event_wait);
    }
}

bool sched_exit_event_pop(pid_t *pid_out) {
    if (!pid_out) {
        return false;
    }

    unsigned long flags = spin_lock_irqsave(&sched_state.exit_events.lock);

    ring_queue_t *r = sched_state.exit_events.ring;
    bool ok = r && ring_queue_pop(r, pid_out);

    spin_unlock_irqrestore(&sched_state.exit_events.lock, flags);

    return ok;
}

u32 sched_exit_event_seq(void) {
    return sched_wait_seq(&sched_state.exit_event_wait);
}

bool sched_exit_event_block_if_unchanged(u32 observed_seq) {
    sched_wait_result_t result = sched_wait_on_queue(
        &sched_state.exit_event_wait,
        observed_seq,
        0,
        SCHED_WAIT_INTERRUPTIBLE
    );
    return result == SCHED_WAIT_WOKEN;
}

u32 sched_poll_wait_seq(void) {
    return sched_wait_seq(&sched_state.poll_wait_queue);
}

bool sched_poll_block_if_unchanged(u32 observed_seq) {
    sched_wait_result_t result = sched_wait_on_queue(
        &sched_state.poll_wait_queue,
        observed_seq,
        0,
        SCHED_WAIT_INTERRUPTIBLE | SCHED_WAIT_POLL_LINK
    );
    return result == SCHED_WAIT_WOKEN;
}

bool sched_poll_block_if_unchanged_until(u32 observed_seq, u64 deadline_tick) {
    sched_wait_result_t result = sched_wait_on_queue(
        &sched_state.poll_wait_queue,
        observed_seq,
        deadline_tick,
        SCHED_WAIT_INTERRUPTIBLE | SCHED_WAIT_POLL_LINK
    );
    return result == SCHED_WAIT_WOKEN || result == SCHED_WAIT_TIMEOUT;
}

void sched_poll_wait(void) {
    if (!sched_running_get()) {
        return;
    }

    u32 seq = sched_poll_wait_seq();
    sched_poll_block_if_unchanged(seq);
}

sched_wait_result_t
sched_wait_deadline(u64 deadline_tick, sched_wait_flags_t flags) {
    u32 observed_seq = sched_wait_seq(&sched_state.sleep_wait_queue);
    return sched_wait_on_queue(
        &sched_state.sleep_wait_queue,
        observed_seq,
        deadline_tick,
        flags
    );
}

void sched_wake_one_locked(sched_wait_queue_t *queue) {
    if (!queue || !queue->list) {
        return;
    }

    __atomic_add_fetch(&queue->wake_seq, 1, __ATOMIC_RELEASE);

    bool wake_queue = sched_queue_has_waiters(queue);
    bool wake_pollers = false;

    if (
        queue != &sched_state.poll_wait_queue && queue->poll_link &&
        sched_state.poll_wait_queue.list
    ) {
        __atomic_add_fetch(&sched_state.poll_wait_queue.wake_seq, 1, __ATOMIC_RELEASE);
        wake_pollers = sched_queue_has_waiters(&sched_state.poll_wait_queue);
    }

    if (!wake_queue && !wake_pollers) {
        return;
    }

    if (wake_queue) {
        wake_queue_one(queue);
    }

    if (wake_pollers) {
        wake_queue_all(&sched_state.poll_wait_queue);
    }
}

void sched_wake_one(sched_wait_queue_t *queue) {
    if (!queue || !queue->list) {
        return;
    }

    unsigned long flags = sched_lock_save();
    sched_wake_one_locked(queue);
    sched_lock_restore(flags);
}

void sched_wake_all(sched_wait_queue_t *queue) {
    if (!queue || !queue->list) {
        return;
    }

    __atomic_add_fetch(&queue->wake_seq, 1, __ATOMIC_RELEASE);

    bool wake_queue = sched_queue_has_waiters(queue);
    bool wake_pollers = false;

    if (
        queue != &sched_state.poll_wait_queue && queue->poll_link &&
        sched_state.poll_wait_queue.list
    ) {
        __atomic_add_fetch(&sched_state.poll_wait_queue.wake_seq, 1, __ATOMIC_RELEASE);
        wake_pollers = sched_queue_has_waiters(&sched_state.poll_wait_queue);
    }

    if (!wake_queue && !wake_pollers) {
        return;
    }

    unsigned long flags = sched_lock_save();

    if (wake_queue) {
        wake_queue_all(queue);
    }

    if (wake_pollers) {
        wake_queue_all(&sched_state.poll_wait_queue);
    }

    sched_lock_restore(flags);
}
