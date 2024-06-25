#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define SERIAL_COM(number) serial_port_map[(number) - 1]

#define SERIAL_DEFAULT_BAUD 9600
#define SERIAL_MAX_BAUD     115200

MAYBE_UNUSED
static usize serial_port_map[] = {
    0x3f8, // COM1
    0x2f8, // COM2
    0x3e8, // etc...
    0x2e8,
    0x5f8,
    0x4f8,
    0x5e8,
    0x4e8
};


bool init_serial(usize port, u32 baud);

void send_serial(usize port, char c);
char receive_serial(usize port);

void send_serial_string(usize port, const char* s);
