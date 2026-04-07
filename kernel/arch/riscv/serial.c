#include "serial.h"

#define UART_THR 0x00
#define UART_RBR 0x00
#define UART_IER 0x01
#define UART_LSR 0x05

#define UART_LSR_RX_READY 0x01
#define UART_LSR_TX_IDLE  0x20
#define UART_IER_RX_READY 0x01

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

bool serial_try_receive(uintptr_t base, char *out) {
    if (!_uart_has_data(base)) {
        return false;
    }

    if (out) {
        *out = receive_serial(base);
    } else {
        (void)receive_serial(base);
    }

    return true;
}

void serial_set_rx_interrupt(uintptr_t base, bool enable) {
    volatile u8 *ier = _uart_reg(base, UART_IER);

    if (enable) {
        *ier |= UART_IER_RX_READY;
    } else {
        *ier &= (u8)~UART_IER_RX_READY;
    }
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
