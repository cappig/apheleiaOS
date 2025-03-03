#pragma once

#include <aos/syscalls.h>

#include "sched/process.h"
#include "sys/cpu.h"


static inline file_desc* get_fd(usize fd) {
    return process_get_fd(cpu->sched->current, fd);
}

static inline bool validate_fd(usize fd) {
    file_desc* fdesc = get_fd(fd);

    if (!fdesc)
        return false;

    return fdesc->node;
}

static inline bool validate_ptr(const void* ptr, usize len, bool write) {
    if (!len)
        return false; // ?

    return process_validate_ptr(cpu->sched->current, ptr, len, write);
}

static inline bool validate_signum(usize signum) {
    if (signum >= SIGNAL_COUNT)
        return false;

    return true;
}

static inline bool has_perms(usize user) {
    usize current_uid = cpu->sched->current->user.euid;

    if (current_uid == SUPERUSER_UID)
        return true;

    if (user == current_uid)
        return true;

    return false;
}


void syscall_init(void);
