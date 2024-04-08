#pragma once

#include <base/types.h>

#define SERIAL_PORT 0x3f8

#define SERIAL_DEFAULT_BAUD 9600
#define SERIAL_MAX_BAUD     115200


void init_serial(u32 baud);

void send_serial(char c);
char receive_serial(void);

void send_serial_string(const char* s);
