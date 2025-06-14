#include "pty.h"

#include <base/attributes.h>
#include <base/types.h>
#include <ctype.h>
#include <data/ring.h>
#include <data/vector.h>
#include <errno.h>
#include <input/kbd.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "arch/lock.h"
#include "mem/heap.h"
#include "sched/scheduler.h"
#include "sched/syscall.h"
#include "sched/wait.h"
#include "sys/cpu.h"
#include "vfs/fs.h"


// FIXME: this is still not ideal, use atomic operations
static bool _pty_wait_for_line(pseudo_tty* pty, bool block) {
    wait_list* sem = pty->waiters;

    spin_lock(&sem->spinlock);

    if (!ring_buffer_is_empty(pty->input_buffer)) {
        sem->passable = true;
        spin_unlock(&sem->spinlock);

        return true;
    }

    sem->passable = false;

    if (!block)
        return false;

    wait_list_append(sem, cpu->scheduler.current);
    spin_unlock(&sem->spinlock);

    while (!sem->passable) {
        // asm volatile("pause");
    }

    return true;
}

static void _pty_signal_line(pseudo_tty* pty) {
    wait_list* sem = pty->waiters;

    spin_lock(&sem->spinlock);

    sem->passable = true;
    wait_list_wake_up(pty->waiters);

    spin_unlock(&sem->spinlock);
}


static inline void _send_out(pseudo_tty* pty, u8 ch) {
    ring_buffer_push(pty->output_buffer, ch);

    if (pty->out_hook)
        pty->out_hook(pty, ch);
}

static bool _process_output(vfs_node* node, u8 ch) {
    pseudo_tty* pty = node->private;
    termios_t* tos = &pty->termios;

    // don't perform any output processing
    if (!(tos->c_oflag & OPOST))
        return true;

    // ignore CR
    if (tos->c_iflag & ONOCR && ch == '\r')
        return false;

    // Map NL to NL-CR
    if (tos->c_iflag & ONLCR && ch == '\n') {
        _send_out(pty, '\n');
        _send_out(pty, '\r');

        return false;
    }

    // Map CR to NL
    if (tos->c_iflag & OCRNL && ch == '\r') {
        _send_out(pty, '\n');
        return false;
    }

    // Map NL to CR
    if (tos->c_iflag & ONLRET && ch == '\n') {
        _send_out(pty, '\r');
        return false;
    }

    return true;
}


static isize slave_write(vfs_node* node, void* buf, usize offset, usize len, u32 flags) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    u8* data = (u8*)buf;

    for (usize i = 0; i < len; i++) {
        u8 ch = data[i];

        if (_process_output(node, ch))
            _send_out(pty, ch);
    }

    return len;
}

static isize slave_read(vfs_node* node, void* buf, UNUSED usize offset, usize len, u32 flags) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    // Possibly block the calling process until new data becomes available
    if (!_pty_wait_for_line(pty, !(flags & VFS_NONBLOCK)))
        return -EAGAIN;

    return ring_buffer_pop_array(pty->input_buffer, buf, len);
}


static usize _flush_line_buffer(pseudo_tty* pty) {
    usize len = pty->line_buffer->size;
    u8* data = pty->line_buffer->data;

    if (!len)
        return 0;

    ring_buffer_push_array(pty->input_buffer, data, len);

    vec_clear(pty->line_buffer);

    return len;
}

static void _erase_preceding(vfs_node* node, usize chars, u32 flags) {
    pseudo_tty* pty = node->private;
    termios_t* tos = &pty->termios;

    for (usize i = 0; i < chars; i++) {
        u8 ch = 0;

        if (!vec_pop(pty->line_buffer, &ch))
            break;

        if (tos->c_lflag & ECHO && tos->c_lflag & ECHOE) {
            // Make sure to delete the caret as well ^_^
            if (!isprint(ch) && ch != '\n')
                slave_write(node, "\b \b", 0, 3, flags);

            slave_write(node, "\b \b", 0, 3, flags);
        }
    }
}


// -1 if a line flush occurred, 1 if ch can be echoed, 0 if ch should not be echoed
static i8 _canonical_input(vfs_node* node, u8 ch, u32 flags) {
    pseudo_tty* pty = node->private;
    termios_t* tos = &pty->termios;

    // Discard the current line
    if (ch == tos->c_cc[VKILL]) {
        vec_clear(pty->line_buffer);

        if (tos->c_lflag & ECHOK) {
            _erase_preceding(node, pty->line_buffer->size, flags);
            return 0;
        }

        return 1;
    }

    // Erase the preveous char
    if (ch == tos->c_cc[VERASE]) {
        if (!pty->line_buffer->size)
            return 0;

        _erase_preceding(node, 1, flags);

        return !(tos->c_lflag & ECHOE);
    }

    vec_push(pty->line_buffer, &ch);

    // Flush the line buffer
    if (ch == '\n' || ch == tos->c_cc[VEOL] || ch == tos->c_cc[VEOF]) {
        _flush_line_buffer(pty);
        return -1;
    }

    return 1;
}

// -1 if ch should be ignored
static i16 _process_input(vfs_node* node, u8 ch) {
    pseudo_tty* pty = node->private;
    termios_t* tos = &pty->termios;

    if (!ch)
        return 0;

    // strip the 7th bit
    if (tos->c_iflag & ISTRIP)
        ch &= 0x8f;

    // this char was escaped, don't parse
    if (pty->next_literal) {
        pty->next_literal = false;
        return ch;
    }

    // send requested signals
    if (tos->c_lflag & ISIG) {
        usize signal = 0;

        if (ch == tos->c_cc[VINTR])
            signal = SIGINT;
        else if (ch == tos->c_cc[VQUIT])
            signal = SIGQUIT;
        else if (ch == tos->c_cc[VSUSP])
            signal = SIGTSTP;

        if (signal) {
            //  TODO: job control, we want to send the signal to the fg process
            // signal_send(cpu->scheduler.current, -1, signal);
            return ch;
        }
    }

    // deprive the next character of any special meaning
    if (ch == tos->c_cc[VLNEXT] && tos->c_lflag & IEXTEN) {
        pty->next_literal = true;
        return ch;
    }

    // ignore carriage returns
    if (tos->c_iflag & IGNCR && ch == '\r')
        return -1;

    // NL <-> CR conversion
    if (tos->c_iflag & ICRNL && ch == '\r')
        ch = '\n';
    else if (tos->c_iflag & INLCR && ch == '\n')
        ch = '\r';

    // map upper case chars to lower case
    if (tos->c_iflag & IUCLC)
        ch = tolower(ch);

    return ch;
}

static void _echo_in(vfs_node* node, u8 ch) {
    pseudo_tty* pty = node->private;
    termios_t* tos = &pty->termios;

    bool printable = isprint(ch) || ch == '\n';
    bool special_caret = tos->c_lflag & ECHOCTL;

    if (printable || !special_caret) {
        slave_write(node, &ch, 0, 1, 0);
    } else {
        // Use caret notation for special chars
        char caret[] = "^X";
        caret[1] = ctrl_to_caret(ch);

        slave_write(node, caret, 0, 2, 0);
    }
}

static isize master_write(vfs_node* node, void* buf, UNUSED usize offset, usize len, u32 flags) {
    pseudo_tty* pty = node->private;
    termios_t* tos = &pty->termios;

    if (!pty || !buf)
        return -1;

    u8* data = (u8*)buf;

    bool has_data = false;

    for (usize i = 0; i < len; i++) {
        i16 ch = _process_input(node, data[i]);

        if (ch <= 0)
            continue;

        bool echo = tos->c_lflag & ECHO;

        // Canonical mode works with lines of text. The text becomes
        // readable once the line buffer gets flushed (once enter is presesed etc.)
        if (tos->c_lflag & ICANON) {
            i8 canon = _canonical_input(node, ch, flags);

            // The line buffer was flushed, we have new data
            if (canon < 0) {
                has_data = true;

                if (tos->c_lflag & ECHONL)
                    echo = true;
            } else {
                echo &= canon;
            }
        } else {
            has_data = true;
        }

        if (echo)
            _echo_in(node, ch);
    }

    if (has_data)
        _pty_signal_line(pty);

    // TODO: implement VMIN and VTIME

    return len;
}

static isize master_read(vfs_node* node, void* buf, UNUSED usize offset, usize len, u32 flags) {
    pseudo_tty* pty = node->private;

    if (!pty || !buf)
        return -1;

    return ring_buffer_pop_array(pty->output_buffer, buf, len);
}


static isize pty_ioctl(vfs_node* node, u64 request, usize arg_len, u64* args) {
    pseudo_tty* pty = node->private;

    if (!pty)
        return -ENOTTY;

    if (!args || !arg_len)
        return -EINVAL;

    switch (request) {

    // Set the winsize
    case TIOCSWINSZ:
        if (!validate_ptr(args, sizeof(winsize_t), false))
            return -EINVAL;

        memcpy(&pty->winsize, args, sizeof(winsize_t));

        // TODO: sigwinch
        return 0;

    // Get the winsize struct
    case TIOCGWINSZ:
        if (!validate_ptr(args, sizeof(winsize_t), true))
            return -EINVAL;

        memcpy(args, &pty->winsize, sizeof(winsize_t));
        return 0;

    // Get the termios struct
    case TCGETS:
        if (!validate_ptr(args, sizeof(termios_t), true))
            return -EINVAL;

        memcpy(args, &pty->termios, sizeof(termios_t));
        return 0;

    // Set the termios struct
    case TCSETSF:
    case TCSETSW: // FIXME: this is not how these should behave
        vec_clear(pty->line_buffer);
        ring_buffer_clear(pty->input_buffer);

        FALLTHROUGH;
    case TCSETS:
        if (!validate_ptr(args, sizeof(termios_t), false))
            return -EINVAL;

        memcpy(&pty->termios, args, sizeof(termios_t));
        return 0;

    default:
        return -EINVAL;
    }
}


pseudo_tty* pty_create(winsize_t* win, usize buffer_size) {
    pseudo_tty* pty = kcalloc(sizeof(pseudo_tty));

    pty->input_buffer = ring_buffer_create(buffer_size);
    pty->output_buffer = ring_buffer_create(buffer_size);

    // Configure the 'window' size
    if (win) {
        memcpy(&pty->winsize, win, sizeof(winsize_t));
    } else {
        pty->winsize.ws_col = 80;
        pty->winsize.ws_row = 25;
    }

    // Configure termios
    __termios_default_init(&pty->termios);

    pty->line_buffer = vec_create_sized(pty->winsize.ws_col, sizeof(char));

    pty->master = vfs_create_node(NULL, VFS_CHARDEV);
    pty->master->interface = kcalloc(sizeof(vfs_node_interface));
    pty->master->interface->read = master_read;
    pty->master->interface->write = master_write;
    pty->master->interface->ioctl = pty_ioctl;
    pty->master->private = pty;

    pty->slave = vfs_create_node(NULL, VFS_CHARDEV);
    pty->slave->interface = kcalloc(sizeof(vfs_node_interface));
    pty->slave->interface->read = slave_read;
    pty->slave->interface->write = slave_write;
    pty->slave->interface->ioctl = pty_ioctl;
    pty->slave->private = pty;

    pty->waiters = wait_list_create();

    pty->next_literal = false;

    return pty;
}

void pty_destroy(pseudo_tty* pty) {
    ring_buffer_destroy(pty->input_buffer);
    ring_buffer_destroy(pty->output_buffer);

    vec_destroy(pty->line_buffer);

    vfs_destroy_node(pty->master);
    vfs_destroy_node(pty->slave);

    kfree(pty);
}
