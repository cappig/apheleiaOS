#pragma once

#include <aos/signals.h>
#include <aos/syscalls.h>
#include <base/types.h>
#include <data/vector.h>
#include <parse/elf.h>
#include <x86/paging.h>

#include "arch/idt.h"
#include "vfs/fs.h"

#define SCHED_KSTACK_PAGES 2
#define SCHED_KSTACK_SIZE  (SCHED_KSTACK_PAGES * PAGE_4KIB)

#define SCHED_USTACK_PAGES 8
#define SCHED_USTACK_SIZE  (SCHED_USTACK_PAGES * PAGE_4KIB)


enum process_state {
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_DONE,
};

enum process_type {
    PROC_USER,
    PROC_KERNEL,
};

typedef struct {
    vfs_node* node;
    usize offset;
} file_desc;

typedef struct {
    u16 pending; // the nth bit being set marks the (n-1)th signal as pending
    u16 masked;

    usize current; // the current signal being handled, 0 if none
    sighandler_t handlers[SIGNAL_COUNT - 1]; // signals are indexed from 1
} process_signals;

typedef struct {
    page_table* mem_map;

    // Points to the entry in the process tree
    tree_node* tree_entry;

    // The user stack
    u64 stack_paddr;
    usize stack_size;
    u64 stack;

    // File descriptor table
    vector* fd_table;
} process_user;

typedef struct {
    char* name;
    usize id;

    u8 state;
    u8 type;
    u8 status; // exit code

    // The kernel stack
    usize stack_size;
    u64 stack;
    u64 stack_ptr;

    // Userland information. Not used by kernel processes
    process_user user;
} process;


process* process_create(const char* name, u8 type, usize pid);

bool process_free(process* proc);
void process_destroy(process* proc);

void process_init_stack(process* parent, process* child);
void process_init_page_map(process* parent, process* child);
void process_init_file_descriptors(process* parent, process* child);

isize process_open_fd(process* proc, const char* path, isize fd);

void process_set_state(process* proc, int_state* state);

process* spawn_kproc(const char* name, void* entry);
process* spawn_uproc(const char* name);

process* process_fork(process* parent);

bool process_exec_elf(process* proc, elf_header* header);
