#pragma once

#include "sched/process.h"
#include "sys/cpu.h"


static inline file_desc* get_fd(usize fd) {
    return process_get_fd(cpu->scheduler.current->proc, fd);
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

    return proc_validate_ptr(cpu->scheduler.current->proc, ptr, len, write);
}

static inline bool validate_signum(usize signum) {
    if (!signum)
        return false;

    if (signum >= NSIG)
        return false;

    return true;
}

static inline bool has_perms(usize user) {
    sched_thread* thread = cpu->scheduler.current;
    usize current_uid = thread->proc->identity.euid;

    if (current_uid == SUPERUSER_UID)
        return true;

    if (user == current_uid)
        return true;

    return false;
}


void syscall_init(void);
