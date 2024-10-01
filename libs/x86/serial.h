#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define SERIAL_DEFAULT_BAUD 9600
#define SERIAL_MAX_BAUD     115200

enum serial_port_map {
    SERIAL_COM1 = 0x3f8,
    SERIAL_COM2 = 0x2f8,
    SERIAL_COM3 = 0x3e8,
    SERIAL_COM4 = 0x2e8,
    SERIAL_COM5 = 0x5f8,
    SERIAL_COM6 = 0x4f8,
    SERIAL_COM7 = 0x5e8,
    SERIAL_COM8 = 0x4e8
};


bool init_serial(usize port, u32 baud);

void send_serial(usize port, char c);
char receive_serial(usize port);

void send_serial_string(usize port, const char* s);
