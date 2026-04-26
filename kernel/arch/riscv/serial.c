#include "serial.h"

#ifdef _KERNEL
#include <sys/lock.h>
static spinlock_t tx_lock = SPINLOCK_INIT;
#endif

#define UART_THR 0x00
#define UART_RBR 0x00
#define UART_IER 0x01
#define UART_LSR 0x05

#define UART_LSR_RX_READY 0x01
#define UART_LSR_TX_IDLE  0x20
#define UART_IER_RX_READY 0x01

static inline u8 _uart_read(uintptr_t base, uintptr_t reg) {
#if RISCV_UART_STRIDE == 4
    return (u8)(*(volatile u32 *)(base + reg * 4));
#else
    return *(volatile u8 *)(base + reg * RISCV_UART_STRIDE);
#endif
}

static inline void _uart_write(uintptr_t base, uintptr_t reg, u8 val) {
#if RISCV_UART_STRIDE == 4
    *(volatile u32 *)(base + reg * 4) = val;
#else
    *(volatile u8 *)(base + reg * RISCV_UART_STRIDE) = val;
#endif
}

static inline bool _uart_has_data(uintptr_t base) {
    return (_uart_read(base, UART_LSR) & UART_LSR_RX_READY) != 0;
}

void send_serial(uintptr_t base, char c) {
    if (!base) {
        return;
    }

#ifdef _KERNEL
    unsigned long flags = spin_lock_irqsave(&tx_lock);
#endif

    if (c == '\n') {
        while ((_uart_read(base, UART_LSR) & UART_LSR_TX_IDLE) == 0) {}
        _uart_write(base, UART_THR, '\r');
    }

    while ((_uart_read(base, UART_LSR) & UART_LSR_TX_IDLE) == 0) {}
    _uart_write(base, UART_THR, (u8)c);

#ifdef _KERNEL
    spin_unlock_irqrestore(&tx_lock, flags);
#endif
}

char receive_serial(uintptr_t base) {
    if (!base) {
        return 0;
    }

    while (!_uart_has_data(base)) {}
    return (char)_uart_read(base, UART_RBR);
}

bool serial_try_receive(uintptr_t base, char *out) {
    if (!base) {
        return false;
    }

    if (!_uart_has_data(base)) {
        return false;
    }

    if (out) {
        *out = (char)_uart_read(base, UART_RBR);
    } else {
        (void)_uart_read(base, UART_RBR);
    }

    return true;
}

void serial_set_rx_interrupt(uintptr_t base, bool enable) {
    if (!base) {
        return;
    }

    u8 ier = _uart_read(base, UART_IER);

    if (enable) {
        ier |= UART_IER_RX_READY;
    } else {
        ier &= (u8)~UART_IER_RX_READY;
    }

    _uart_write(base, UART_IER, ier);
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
