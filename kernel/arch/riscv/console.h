#pragma once

#include <base/types.h>

void riscv_console_backend_init(uintptr_t uart_base);
void riscv_console_set_uart_base(uintptr_t uart_base);
uintptr_t riscv_console_uart_base(void);
void riscv_console_set_output_suppressed(bool suppressed);
