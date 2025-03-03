#include "serial.h"

#include "asm.h"

void init_serial(usize port, u8 line, u32 baud) {
    outb(port + 1, 0x00);
    outb(port + 3, 0x80);

    u16 divisor = SERIAL_MAX_BAUD / baud;
    outb(port + 0, divisor & 0xff);
    outb(port + 1, (divisor >> 8) & 0xff);

    outb(port + 3, line & 0x8f);
    outb(port + 2, 0xc7); // Set the FIFO to 14 bytes

    outb(port + 4, 0x0f);
    outb(port + 1, 0x01); // enable interrupts
}

bool check_serial(usize port) {
    u8 modem_reg = inb(port + 4);

    // loopback mode
    outb(port + 4, 0x1e);
    outb(port + 0, 0xae);

    if (inb(port + 0) != 0xae)
        return false;

    outb(port + 4, modem_reg);

    return true;
}

void send_serial(usize port, char c) {
    while (!(inb(port + 5) & 0x20))
        continue;

    outb(port, c);
}

char receive_serial(usize port) {
    while (!(inb(port + 5) & 0x01))
        continue;

    return inb(port);
}

void send_serial_string(usize port, const char* s) {
    while (*s)
        send_serial(port, *s++);
}

void send_serial_sized_string(usize port, const char* s, usize len) {
    for (usize i = 0; i < len; i++)
        send_serial(port, s[i]);
}
