#include "serial.h"

#include <x86/asm.h>

void init_serial(size_t port, u8 line, u32 baud) {
    outb(port + SERIAL_INTERRUPT_ENABLE, 0x00);
    outb(port + SERIAL_LINE_CONTROL, 0x80); // set the divisor latch access bit

    u16 divisor = SERIAL_MAX_BAUD / baud;

    outb(port, divisor & 0xff);
    outb(port + 1, (divisor >> 8) & 0xff);

    outb(port + SERIAL_LINE_CONTROL, line & 0x8f);
    outb(port + SERIAL_FIFO_CONTROL, 0xc7); // flush FIFOs, set threshold at 14 bytes

    outb(port + SERIAL_MODEM_CONTROL, 0x0b); // enable RTS/DTR
}

bool test_serial(size_t port) {
    u8 modem_reg = inb(port + SERIAL_MODEM_CONTROL);

    outb(port + 4, 0x1e); // loopback mode

    outb(port + SERIAL_OUT_BUFFER, 0xae);

    if (inb(port + SERIAL_IN_BUFFER) != 0xae) {
        return false;
    }

    outb(port + SERIAL_MODEM_CONTROL, modem_reg);

    return true;
}


void send_serial(size_t port, char c) {
    while (!(inb(port + 5) & 0x20)) {
        continue;
    }

    outb(port, c);
}

char receive_serial(size_t port) {
    while (!(inb(port + 5) & 0x01)) {
        continue;
    }

    return inb(port);
}

void send_serial_string(size_t port, const char *s) {
    while (*s) {
        send_serial(port, *s++);
    }
}

void send_serial_sized_string(size_t port, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        send_serial(port, s[i]);
    }
}

bool serial_has_data(size_t port) {
    return (inb(port + 5) & 0x01) != 0;
}

bool serial_try_receive(size_t port, char *out) {
    if (!out) {
        return false;
    }

    if (!serial_has_data(port)) {
        return false;
    }

    *out = inb(port);
    return true;
}
