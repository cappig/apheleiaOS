#include "serial.h"

#include <base/types.h>
#include <data/tree.h>
#include <string.h>
#include <x86/serial.h>

#include "arch/irq.h"
#include "base/attributes.h"
#include "sys/panic.h"
#include "vfs/fs.h"
#include "vfs/pty.h"
#include "x86/asm.h"

#define COM_PORT_COUNT 8

static usize com_port[COM_PORT_COUNT] = {
    SERIAL_COM1,
    SERIAL_COM2,
    SERIAL_COM3,
    SERIAL_COM4,
    SERIAL_COM5,
    SERIAL_COM6,
    SERIAL_COM7,
    SERIAL_COM8,
};

static pseudo_tty* ptys[COM_PORT_COUNT] = {};


void serial_output(usize port, u8* data, usize len) {
    port -= 1;

    assert(port <= COM_PORT_COUNT);

    pseudo_tty* pty = ptys[port];

    if (!pty)
        return;

    vfs_node* node = pty->slave;

    vfs_write(node, data, 0, len, 0);
}

static void _read(pseudo_tty* term, void* buf, usize len) {
    // TODO: implement this
}

static void _write(pseudo_tty* term, u16 ch) {
    usize port = (usize)term->private;
    send_serial(port, ch);
}

bool init_serial_port(u8 index) {
    assert(index <= COM_PORT_COUNT);

    usize port = com_port[index];

    if (!check_serial(port))
        return false;

    pseudo_tty* pty = pty_create(NULL, SERIAL_DEV_BUFFER_SIZE);

    if (!pty_hook_serial(pty, port))
        return false;

    char name[] = "com0";
    name[3] += index;

    pty->slave->name = strdup(name);

    ptys[index] = pty;

    vfs_node* dev = vfs_open("/dev", VFS_DIR, true, KDIR_MODE);
    vfs_insert_child(dev, pty->slave);

    return true;
}

bool pty_hook_serial(pseudo_tty* pty, usize port) {
    if (!pty)
        return false;

    termios_t* tos = &pty->termios;

    if (!tos)
        return false;


    if (!tos->c_ospeed)
        tos->c_ospeed = SERIAL_DEFAULT_BAUD;

    if (tos->c_ospeed > SERIAL_MAX_BAUD)
        tos->c_ospeed = SERIAL_MAX_BAUD;

    // construct the line control byte
    // https://wiki.osdev.org/Serial_Ports#Line_Protocol
    u8 line_reg = 0;

    line_reg = tos->c_cflag & CSIZE; // control the size of a transimitted 'byte' 5,6,7 or 8
    line_reg |= (tos->c_cflag & CSTOPB) << 2; // set if 2 stop bits instead of 1

    if (tos->c_cflag & PARENB) {
        line_reg |= 1 << 3; // set if parity bits should be generated
        line_reg |= (!(tos->c_cflag & PARODD)) << 4; // set if even parity
        line_reg |= (tos->c_cflag & CMSPAR) << 5; // instead of even/odd parity use mark/space
    }

    init_serial(port, line_reg, tos->c_ospeed);

    pty->private = (void*)port; // uuuhhh, kind of fucked up

    pty->out_hook = _write;

    return true;
}

static i16 _get_data(usize port_index) {
    usize port = com_port[port_index - 1];

    u8 line = inb(port + SREG_LINE_STATUS);

    if (!(line & 1))
        return -1;

    u8 ch = inb(port + SREG_IN_BUFFER);

    pseudo_tty* pty = ptys[port_index - 1];

    if (pty)
        vfs_write(pty->master, &ch, 0, 1, 0);

    return ch;
}

static void _serial_irq1(UNUSED int_state* s) {
    _get_data(1);
    _get_data(3);

    irq_ack(IRQ_COM1);
}

static void _serial_irq2(UNUSED int_state* s) {
    _get_data(2);
    _get_data(4);

    irq_ack(IRQ_COM2);
}


void init_serial_dev() {
    irq_register(IRQ_COM1, _serial_irq1);
    irq_register(IRQ_COM2, _serial_irq2);

    init_serial_port(0);
    init_serial_port(1);
    init_serial_port(2);
    init_serial_port(3);
}
