#pragma once

#include <base/types.h>

// qemu virt machine uart; override with RISCV_BOARD=
#ifndef SERIAL_UART0
#define SERIAL_UART0 0x10000000UL
#endif

void send_serial(uintptr_t base, char c);
char receive_serial(uintptr_t base);
bool serial_try_receive(uintptr_t base, char *out);
void serial_set_rx_interrupt(uintptr_t base, bool enable);

void send_serial_string(uintptr_t base, const char *s);
void send_serial_sized_string(uintptr_t base, const char *s, size_t len);
