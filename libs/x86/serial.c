#include "serial.h"

#include "asm.h"


bool init_serial(usize port, u32 baud) {
    outb(port + 1, 0x00);
    outb(port + 3, 0x80);

    u16 divisor = SERIAL_MAX_BAUD / baud;
    outb(port + 0, divisor & 0xff);
    outb(port + 1, (divisor >> 8) & 0xff);

    outb(port + 3, 0x03);
    outb(port + 2, 0xc7);
    outb(port + 4, 0x0b);

    // Check if serial is faulty
    outb(port + 4, 0x1e);
    outb(port + 0, 0xae);
    if (inb(port + 0) != 0xae)
        return false;

    outb(port + 4, 0x0f);
    return true;
}

void send_serial(usize port, char c) {
    // These empty loops are not ideal but eehhh it works
    while ((inb(port + 5) & 0x20) == 0)
        continue;

    outb(port, c);
}

char receive_serial(usize port) {
    while ((inb(port + 5) & 0x01) == 0)
        continue;

    return inb(port);
}

void send_serial_string(usize port, const char* s) {
    while (*s)
        send_serial(port, *s++);
}
