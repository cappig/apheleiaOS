#pragma once

#include <base/types.h>

#ifndef SERIAL_UART0
#define SERIAL_UART0 0x10000000UL
#endif

#ifndef RISCV_UART_STRIDE
#define RISCV_UART_STRIDE 1
#endif

void serial_set_reg_stride(uintptr_t stride);
uintptr_t serial_reg_stride(void);
void serial_set_reg_io_width(uintptr_t width);
uintptr_t serial_reg_io_width(void);

void send_serial(uintptr_t base, char c);
char receive_serial(uintptr_t base);
bool serial_try_receive(uintptr_t base, char *out);
void serial_set_rx_interrupt(uintptr_t base, bool enable);

void send_serial_string(uintptr_t base, const char *s);
void send_serial_buf(uintptr_t base, const char *s, size_t len);
