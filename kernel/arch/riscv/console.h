#pragma once

#include <base/types.h>

void uart_console_init(uintptr_t uart_base);
void uart_console_set_base(uintptr_t uart_base);
uintptr_t uart_console_base(void);
void uart_console_set_suppressed(bool suppressed);
