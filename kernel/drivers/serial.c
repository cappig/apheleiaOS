#include "serial.h"

#include <x86/serial.h>

#include "base/types.h"
#include "string.h"
#include "vfs/fs.h"
#include "vfs/pty.h"

// Should we try to check the EBDA?
static usize com_port[] = {
    SERIAL_COM1,
    SERIAL_COM2,
    SERIAL_COM3,
    SERIAL_COM4,
};

static void _read(vfs_pty* term, void* buf, usize len) {
    // TODO: implement this
}

static void _write(vfs_pty* term, void* buf, usize len) {
    usize port = (usize)term->private;
    char* char_buf = (char*)buf;

    for (usize i = 0; i < len; i++)
        send_serial(port, char_buf[i]);
}

static void _init_port(virtual_fs* vfs, u8 index) {
    vfs_pty* pty = pty_create(SERIAL_DEV_BUFFER_SIZE);
    pty->private = (void*)com_port[index - 1]; // bit hacky but it works
    pty->out_hook = _write;

    char name[] = "ttyS0";
    name[4] += index;

    pty->slave->name = strdup(name);

    vfs_mount(vfs, "/dev", tree_create_node(pty->slave));
}

void init_serial_dev(virtual_fs* vfs) {
    _init_port(vfs, 1);
    // _init_port(vfs, 2);
    // _init_port(vfs, 3);
    // _init_port(vfs, 4);
}
