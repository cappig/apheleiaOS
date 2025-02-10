#include "wait.h"

#include "mem/heap.h"
#include "sys/cpu.h"


wait_list* wait_list_create() {
    wait_list* list = kcalloc(sizeof(wait_list));

    list->procs = vec_create(sizeof(process*));

    return list;
}

bool wait_list_append(wait_list* list, process* proc) {
    if (!cpu->sched_running)
        return false;

    vec_push(list->procs, &proc);

    proc->state = PROC_BLOCKED;
    cpu->sched->proc_ticks_left = 0;

    return true;
}

bool wait_list_wake_up(wait_list* list) {
    if (!cpu->sched_running)
        return false;

    for (;;) {
        process* proc;

        if (!vec_pop(list->procs, &proc))
            break;

        proc->state = PROC_RUNNING;
        cpu->sched->proc_ticks_left = 0;
    }

    return true;
}

void wait_list_destroy(wait_list* list) {
    vec_destroy(list->procs);
    kfree(list);
}
