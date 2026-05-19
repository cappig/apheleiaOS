#pragma once

#ifndef TIMER_FREQ
#define TIMER_FREQ 100
#endif

#ifndef MAX_CORES
#define MAX_CORES 64
#endif

#ifndef SCHED_FD_MAX
#define SCHED_FD_MAX 256
#endif

#ifndef SCHED_GROUP_MAX
#define SCHED_GROUP_MAX 16
#endif

#ifndef SCHED_PIPE_CAPACITY
#define SCHED_PIPE_CAPACITY 4096
#endif

#ifndef POLL_MAX_FDS
#define POLL_MAX_FDS 1024
#endif

#ifndef USER_STACK_PAGES
#define USER_STACK_PAGES 64
#endif

#ifndef EXEC_MAX_ARGS
#define EXEC_MAX_ARGS 16
#endif

#ifndef EXEC_MAX_ARG_LEN
#define EXEC_MAX_ARG_LEN 128
#endif

#ifndef EXEC_MAX_ENV
#define EXEC_MAX_ENV 32
#endif

#ifndef EXEC_MAX_ENV_LEN
#define EXEC_MAX_ENV_LEN 128
#endif

#ifndef SCHED_STACK_SIZE
#define SCHED_STACK_SIZE (64 * 1024)
#endif

#ifndef SCHED_LATENCY_NS
#define SCHED_LATENCY_NS 12000000ULL
#endif

#ifndef SCHED_MIN_GRANULARITY_NS
#define SCHED_MIN_GRANULARITY_NS 1000000ULL
#endif

#ifndef SCHED_REBALANCE_TICKS
#define SCHED_REBALANCE_TICKS 32ULL
#endif

#ifndef SCHED_RQ_CAPACITY
#define SCHED_RQ_CAPACITY 4096
#endif

#ifndef SCHED_SLEEP_HEAP_CAPACITY
#define SCHED_SLEEP_HEAP_CAPACITY SCHED_RQ_CAPACITY
#endif

#ifndef SCHED_IDLE_STEAL_BATCH
#define SCHED_IDLE_STEAL_BATCH 2
#endif

#ifndef SCHED_PUSH_BATCH
#define SCHED_PUSH_BATCH 2
#endif

#ifndef SCHED_EXIT_EVENT_CAP
#define SCHED_EXIT_EVENT_CAP 512
#endif

#ifndef SCHED_WAKE_LOCAL_LOAD_SLOP
#define SCHED_WAKE_LOCAL_LOAD_SLOP 2
#endif

#ifndef KERNEL_HEAP_BLOCK_SIZE
#define KERNEL_HEAP_BLOCK_SIZE 8
#endif

#ifndef KERNEL_HEAP_PAGES
#define KERNEL_HEAP_PAGES 512
#endif

#ifndef KERNEL_HEAP_MAX_ARENAS
#define KERNEL_HEAP_MAX_ARENAS 16
#endif

#ifndef TTY_COUNT
#define TTY_COUNT 4
#endif

#ifndef TTY_INPUT_BUFFER_SIZE
#define TTY_INPUT_BUFFER_SIZE 1024
#endif

#ifndef PTY_COUNT
#define PTY_COUNT 8
#endif

#ifndef PTY_BUFFER_SIZE
#define PTY_BUFFER_SIZE 4096
#endif

#ifndef KBD_DEV_BUFFER_SIZE
#define KBD_DEV_BUFFER_SIZE 256
#endif

#ifndef MOUSE_DEV_BUFFER_SIZE
#define MOUSE_DEV_BUFFER_SIZE 256
#endif

#ifndef WS_MGR_QUEUE_INIT_CAP
#define WS_MGR_QUEUE_INIT_CAP 1024
#endif

#ifndef WS_EV_QUEUE_INIT_CAP
#define WS_EV_QUEUE_INIT_CAP 256
#endif

#ifndef WS_MAX_FB_BYTES
#define WS_MAX_FB_BYTES (16 * 1024 * 1024)
#endif

#ifndef WS_WINDOW_INIT_CAP
#define WS_WINDOW_INIT_CAP 256
#endif

#if TIMER_FREQ < 1
#error "TIMER_FREQ must be at least 1"
#endif

#if MAX_CORES < 1 || MAX_CORES > 64
#error "MAX_CORES must be between 1 and 64"
#endif

#if SCHED_FD_MAX < 3
#error "SCHED_FD_MAX must be at least 3"
#endif

#if SCHED_GROUP_MAX < 1
#error "SCHED_GROUP_MAX must be at least 1"
#endif

#if SCHED_PIPE_CAPACITY < 1
#error "SCHED_PIPE_CAPACITY must be at least 1"
#endif

#if POLL_MAX_FDS < 1
#error "POLL_MAX_FDS must be at least 1"
#endif

#if USER_STACK_PAGES < 1
#error "USER_STACK_PAGES must be at least 1"
#endif

#if EXEC_MAX_ARGS < 1 || EXEC_MAX_ENV < 1
#error "exec vector limits must be at least 1"
#endif

#if EXEC_MAX_ARG_LEN < 2 || EXEC_MAX_ENV_LEN < 2
#error "exec string limits must leave room for a terminator"
#endif

#if SCHED_RQ_CAPACITY < 1 || SCHED_SLEEP_HEAP_CAPACITY < 1
#error "scheduler queue capacities must be at least 1"
#endif

#if SCHED_REBALANCE_TICKS < 1
#error "SCHED_REBALANCE_TICKS must be at least 1"
#endif

#if KERNEL_HEAP_BLOCK_SIZE < 1 || KERNEL_HEAP_PAGES < 1
#error "kernel heap sizing must be non-zero"
#endif

#if KERNEL_HEAP_MAX_ARENAS < 1
#error "KERNEL_HEAP_MAX_ARENAS must be at least 1"
#endif

#if TTY_COUNT < 1 || PTY_COUNT < 1
#error "terminal counts must be non-zero"
#endif

#if TTY_INPUT_BUFFER_SIZE < 2 || PTY_BUFFER_SIZE < 1
#error "terminal buffers must be non-zero"
#endif

#if KBD_DEV_BUFFER_SIZE < 1 || MOUSE_DEV_BUFFER_SIZE < 1
#error "input device buffers must be non-zero"
#endif

#if WS_MGR_QUEUE_INIT_CAP < 1 || WS_EV_QUEUE_INIT_CAP < 1
#error "window-server queue capacities must be non-zero"
#endif

#if WS_MAX_FB_BYTES < 4096 || WS_WINDOW_INIT_CAP < 1
#error "window-server limits are too small"
#endif
