#include "serial.h"

#define UART_THR 0x00
#define UART_RBR 0x00
#define UART_LSR 0x05

#define UART_LSR_RX_READY 0x01
#define UART_LSR_TX_IDLE  0x20

static inline volatile u8 *_uart_reg(uintptr_t base, uintptr_t reg) {
    return (volatile u8 *)(base + reg);
}

static inline bool _uart_has_data(uintptr_t base) {
    volatile u8 *lsr = _uart_reg(base, UART_LSR);
    return (*lsr & UART_LSR_RX_READY) != 0;
}

void send_serial(uintptr_t base, char c) {
    volatile u8 *lsr = _uart_reg(base, UART_LSR);
    volatile u8 *thr = _uart_reg(base, UART_THR);

    while ((*lsr & UART_LSR_TX_IDLE) == 0) {
        continue;
    }

    *thr = (u8)c;
}

char receive_serial(uintptr_t base) {
    volatile u8 *rbr = _uart_reg(base, UART_RBR);

    while (!_uart_has_data(base)) {
        continue;
    }

    return (char)(*rbr);
}

void send_serial_string(uintptr_t base, const char *s) {
    while (*s) {
        send_serial(base, *s++);
    }
}

void send_serial_sized_string(uintptr_t base, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        send_serial(base, s[i]);
    }
}
