#include "console.h"

#include <base/types.h>
#include <data/ring.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <term/term.h>
#include <x86/serial.h>

#include "vfs/fs.h"

static terminal* console_term;
static usize com_port = 0;


static isize _write(UNUSED vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    kputsn(buf, len);

    return len;
}

void kputsn(const char* str, usize len) {
    if (com_port)
        send_serial_string(com_port, str);

    if (console_term)
        term_parse(console_term, str, len);
}

void kputs(const char* str) {
    kputsn(str, (usize)-1);
}

int kprintf(char* fmt, ...) {
    char buf[KPRINTF_BUF_SIZE];

    va_list args;
    va_start(args, fmt);

    int ret = vsprintf(buf, fmt, args);

    kputs(buf);

    va_end(args);

    return ret;
}


void init_console(virtual_fs* vfs, terminal* term) {
    console_term = term;

    vfs_node* node = vfs_create_node("console", VFS_CHARDEV);
    node->interface = vfs_create_file_interface(NULL, _write);

    vfs_mount(vfs, "/dev", tree_create_node(node));
}


void console_set_write(term_putc_fn write_fn) {
    console_term->putc_fn = write_fn;
}

void console_set_serial(usize port) {
    com_port = port;
}
