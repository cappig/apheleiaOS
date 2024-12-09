#pragma once

#include <base/types.h>
#include <parse/elf.h>
#include <x86/paging.h>

#include "arch/idt.h"
#include "vfs/fs.h"

#define SCHED_KSTACK_PAGES 2
#define SCHED_KSTAK_SIZE   (SCHED_KSTACK_PAGES * PAGE_4KIB)

#define SCHED_USTACK_PAGES 4
#define SCHED_USTACK_SIZE  (SCHED_USTACK_PAGES * PAGE_4KIB)

#define SCHED_STATE_OFFSET (SCHED_KSTAK_SIZE - sizeof(int_state))

#define FD_TABLE_SIZE 20

enum std_streams {
    FD_STDIN = 0,
    FD_STDOUT = 1,
    FD_STDERR = 2,
};

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
    char* name;
    usize id;

    u8 state;
    u8 type;

    page_table* cr3;

    u8* kernel_stack;
    u8* ksp;

    // The fields bellow are not used by kernel processes
    usize user_stack_size;
    u8* user_stack;

    vfs_node* fds[FD_TABLE_SIZE];
} process;


process* process_create(const char* name, u8 type, usize pid);
void process_destroy(process* proc);

void process_init_stack(process* parent, process* child);
void process_init_page_map(process* parent, process* child);

void process_set_state(process* proc, int_state* state);

process* spawn_kproc(const char* name, void* entry);
process* spawn_uproc(const char* name);

bool process_exec_elf(process* proc, elf_header* header);
