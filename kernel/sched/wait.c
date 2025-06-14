#include "wait.h"

#include <data/list.h>

#include "arch/lock.h"
#include "mem/heap.h"
#include "sched/process.h"
#include "sys/cpu.h"


wait_list* wait_list_create() {
    wait_list* list = kcalloc(sizeof(wait_list));

    // list->list = (linked_list){0};
    list->spinlock = SPINLOCK_UNLOCKED;

    return list;
}

void wait_list_destroy(wait_list* list) {
    list_destroy(&list->list, false);
}


bool wait_list_append(wait_list* sem, sched_thread* thread) {
    if (!cpu->scheduler.running)
        return false;

    thread->state = T_SLEEPING;

    sched_dequeue(thread, true);

    list_append(&sem->list, &thread->lnode);

    cpu->scheduler.needs_resched = true;

    return true;
}

bool wait_list_wake_up(wait_list* list) {
    if (!cpu->scheduler.running)
        return false;

    list_node* node = list->list.head;
    while (node) {
        sched_thread* thread = node->data;
        list_node* next = node->next;

        list_remove(&list->list, node);
        sched_enqueue(thread);

        node = next;
    }

    cpu->scheduler.needs_resched = true;

    return true;
}
