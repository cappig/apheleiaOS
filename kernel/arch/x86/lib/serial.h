#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define SERIAL_DEFAULT_BAUD 9600
#define SERIAL_MAX_BAUD     115200

// 8 data bits, no parity, one stop bit
#define SERAIL_DEFAULT_LINE 0x03

enum serial_port_map {
    SERIAL_COM1 = 0x3f8,
    SERIAL_COM2 = 0x2f8,
    SERIAL_COM3 = 0x3e8,
    SERIAL_COM4 = 0x2e8,
    SERIAL_COM5 = 0x5f8,
    SERIAL_COM6 = 0x4f8,
    SERIAL_COM7 = 0x5e8,
    SERIAL_COM8 = 0x4e8,
};

enum serial_registers {
    SERIAL_IN_BUFFER = 0, // read
    SERIAL_OUT_BUFFER = 0, // write
    SERIAL_INTERRUPT_ENABLE = 1, // read/write
    SERIAL_INTERRUPT_ID = 2, // read
    SERIAL_FIFO_CONTROL = 2, // write
    SERIAL_LINE_CONTROL = 3, // read/write
    SERIAL_MODEM_CONTROL = 4, // read/write
    SERIAL_LINE_STATUS = 5, // read
    SERIAL_MODEM_STATUS = 6, // read
    SERIAL_SCRATCH = 7, // read/write
};


void init_serial(usize port, u8 line, u32 baud);
bool test_serial(usize port);

void send_serial(usize port, char c);
char receive_serial(usize port);

void send_serial_string(usize port, const char* s);
void send_serial_sized_string(usize port, const char* s, usize len);
