#include "serial.h"

#include <base/types.h>
#include <data/tree.h>
#include <string.h>
#include <x86/serial.h>

#include "sys/panic.h"
#include "vfs/fs.h"
#include "vfs/pty.h"

// Should we try to check the EBDA?
static usize com_port[] = {
    SERIAL_COM1,
    SERIAL_COM2,
    SERIAL_COM3,
    SERIAL_COM4,
};


static void _read(pseudo_tty* term, void* buf, usize len) {
    // TODO: implement this
}

static void _write(pseudo_tty* term, void* buf, usize len) {
    usize port = (usize)term->private;
    char* char_buf = (char*)buf;

    for (usize i = 0; i < len; i++)
        send_serial(port, char_buf[i]);
}

bool init_serial_port(u8 index) {
    assert(index < 10);

    usize port = com_port[index];

    if (!init_serial(port, SERIAL_DEFAULT_BAUD))
        return false;

    pseudo_tty* pty = pty_create(SERIAL_DEV_BUFFER_SIZE);

    pty->private = (void*)port;
    pty->out_hook = _write;

    char name[] = "com0";
    name[3] += index;

    pty->slave->name = strdup(name);

    vfs_node* dev = vfs_lookup("/dev");
    vfs_insert_child(dev, pty->slave);

    return true;
}

void init_serial_dev() {
    init_serial_port(0);
    init_serial_port(1);
    init_serial_port(2);
    init_serial_port(3);
}
