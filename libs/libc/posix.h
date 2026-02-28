#pragma once

#ifndef _POSIX_SOURCE
#define _POSIX_SOURCE 1
#endif

#if !defined(_POSIX_C_SOURCE) && defined(_XOPEN_SOURCE)
#if _XOPEN_SOURCE >= 700
#define _POSIX_C_SOURCE 200809L
#elif _XOPEN_SOURCE >= 600
#define _POSIX_C_SOURCE 200112L
#else
#define _POSIX_C_SOURCE 199506L
#endif
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#define _POSIX_VERSION  200809L
#define _POSIX2_VERSION 200809L

#define _POSIX_ADVISORY_INFO       -1
#define _POSIX_ASYNCHRONOUS_IO     -1
#define _POSIX_BARRIERS            -1
#define _POSIX_CHOWN_RESTRICTED    1
#define _POSIX_CLOCK_SELECTION     -1
#define _POSIX_CPUTIME             -1
#define _POSIX_FSYNC               -1
#define _POSIX_JOB_CONTROL         200809L
#define _POSIX_MAPPED_FILES        200809L
#define _POSIX_MEMLOCK             -1
#define _POSIX_MEMLOCK_RANGE       -1
#define _POSIX_MEMORY_PROTECTION   200809L
#define _POSIX_MESSAGE_PASSING     -1
#define _POSIX_MONOTONIC_CLOCK     200809L
#define _POSIX_NO_TRUNC            1
#define _POSIX_PRIORITIZED_IO      -1
#define _POSIX_PRIORITY_SCHEDULING -1
#define _POSIX_RAW_SOCKETS         -1
#define _POSIX_READER_WRITER_LOCKS -1
#define _POSIX_REALTIME_SIGNALS    -1
#define _POSIX_REGEXP              -1
#define _POSIX_SAVED_IDS           -1
#define _POSIX_SEMAPHORES          -1
#define _POSIX_SHARED_MEMORY_OBJECTS -1
#define _POSIX_SHELL               -1
#define _POSIX_SPAWN               -1
#define _POSIX_SPIN_LOCKS          -1
#define _POSIX_SYNCHRONIZED_IO     -1
#define _POSIX_THREADS             -1
#define _POSIX_THREAD_ATTR_STACKADDR -1
#define _POSIX_THREAD_ATTR_STACKSIZE -1
#define _POSIX_THREAD_CPUTIME      -1
#define _POSIX_THREAD_PRIORITY_SCHEDULING -1
#define _POSIX_THREAD_PROCESS_SHARED -1
#define _POSIX_THREAD_SAFE_FUNCTIONS -1
#define _POSIX_TIMERS              200809L
#define _POSIX_TRACE               -1
#define _POSIX_TYPED_MEMORY_OBJECTS -1
#define _POSIX_VDISABLE            '\0'

