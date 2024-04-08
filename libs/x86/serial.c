#include "serial.h"

#include "asm.h"


void init_serial(u32 baud) {
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x80);

    u16 divisor = SERIAL_MAX_BAUD / baud;
    outb(SERIAL_PORT + 0, divisor & 0xff);
    outb(SERIAL_PORT + 1, (divisor >> 8) & 0xff);

    outb(SERIAL_PORT + 3, 0x03);
    outb(SERIAL_PORT + 2, 0xc7);
    outb(SERIAL_PORT + 4, 0x0b);
}

void send_serial(char c) {
    // These empty loops are not ideal but eehhh it works
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0)
        continue;

    outb(SERIAL_PORT, c);
}

char receive_serial(void) {
    while ((inb(SERIAL_PORT + 5) & 0x01) == 0)
        continue;

    return inb(SERIAL_PORT);
}

void send_serial_string(const char* s) {
    while (*s)
        send_serial(*s++);
}
