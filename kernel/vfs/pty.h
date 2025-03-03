#pragma once

#include <data/ring.h>
#include <data/vector.h>
#include <term/termios.h>

#include "fs.h"
#include "sched/wait.h"

typedef struct pseudo_tty pseudo_tty;

typedef void (*pty_hook_fn)(pseudo_tty* term, u16 ch);

typedef struct pseudo_tty {
    // write to input, read from output
    vfs_node* master;

    // write to output, read from input
    vfs_node* slave;

    ring_buffer* input_buffer;
    ring_buffer* output_buffer;

    termios_t termios;
    winsize_t winsize;

    bool next_literal; // the next input character should not be interpreted as a control character
    vector* line_buffer; // Used by canonical mode

    // Hooks that can be called when there is new data in the buffer, optional
    pty_hook_fn out_hook; // There is new data in the output_buffer, the master can read
    pty_hook_fn in_hook; // There is new data in the input_buffer, the slave can read

    wait_list* waiters;

    void* private;
} pseudo_tty;


pseudo_tty* pty_create(winsize_t* win, usize buffer_size);

void pty_destroy(pseudo_tty* pty);
