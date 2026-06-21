#include "serial.h"

#include <string.h>

#ifdef _KERNEL
#include <sys/lock.h>
#endif

#define UART_THR 0x00
#define UART_RBR 0x00
#define UART_IER 0x01
#define UART_LSR 0x05

#define UART_LSR_RX_READY 0x01
#define UART_LSR_TX_IDLE  0x20
#define UART_IER_RX_READY 0x01

#define UART_TX_IRQ_CHUNK 16

typedef struct {
    uintptr_t stride;
    uintptr_t io_width;
#ifdef _KERNEL
    spinlock_t tx_lock;
#endif
} serial_state_t;

static serial_state_t serial = {
    .stride = RISCV_UART_STRIDE,
    .io_width = 1,
#ifdef _KERNEL
    .tx_lock = SPINLOCK_INIT,
#endif
};

static inline uintptr_t _uart_reg(uintptr_t base, uintptr_t reg) {
    return base + reg * serial.stride;
}

static inline u8 _uart_read(uintptr_t base, uintptr_t reg) {
    uintptr_t addr = _uart_reg(base, reg);

    switch (serial.io_width) {
    case 2:
        return (u8)*(volatile u16 *)addr;
    case 4:
        return (u8)*(volatile u32 *)addr;
    case 8:
        return (u8)*(volatile u64 *)addr;
    default:
        return *(volatile u8 *)addr;
    }
}

static inline void _uart_write(uintptr_t base, uintptr_t reg, u8 val) {
    uintptr_t addr = _uart_reg(base, reg);

    switch (serial.io_width) {
    case 2:
        *(volatile u16 *)addr = val;
        break;
    case 4:
        *(volatile u32 *)addr = val;
        break;
    case 8:
        *(volatile u64 *)addr = val;
        break;
    default:
        *(volatile u8 *)addr = val;
        break;
    }
}

static inline bool _uart_has_data(uintptr_t base) {
    return (_uart_read(base, UART_LSR) & UART_LSR_RX_READY) != 0;
}

void serial_set_reg_stride(uintptr_t stride) {
    if (stride != 1 && stride != 2 && stride != 4 && stride != 8) {
        stride = RISCV_UART_STRIDE;
    }

    serial.stride = stride ? stride : 1;
}

uintptr_t serial_reg_stride(void) {
    return serial.stride;
}

void serial_set_reg_io_width(uintptr_t width) {
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        width = 1;
    }

    serial.io_width = width;
}

uintptr_t serial_reg_io_width(void) {
    return serial.io_width;
}

static void _send_serial_unlocked(uintptr_t base, char c) {
    if (c == '\n') {
        while ((_uart_read(base, UART_LSR) & UART_LSR_TX_IDLE) == 0) {}
        _uart_write(base, UART_THR, '\r');
    }

    while ((_uart_read(base, UART_LSR) & UART_LSR_TX_IDLE) == 0) {}
    _uart_write(base, UART_THR, (u8)c);
}

void send_serial(uintptr_t base, char c) {
    if (!base) {
        return;
    }

#ifdef _KERNEL
    unsigned long flags = spin_lock_irqsave(&serial.tx_lock);
#endif

    _send_serial_unlocked(base, c);

#ifdef _KERNEL
    spin_unlock_irqrestore(&serial.tx_lock, flags);
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
    send_serial_buf(base, s, s ? strlen(s) : 0);
}

void send_serial_buf(uintptr_t base, const char *s, size_t len) {
    if (!base || !s || !len) {
        return;
    }

    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > UART_TX_IRQ_CHUNK) {
            chunk = UART_TX_IRQ_CHUNK;
        }

#ifdef _KERNEL
        unsigned long flags = spin_lock_irqsave(&serial.tx_lock);
#endif

        for (size_t i = 0; i < chunk; i++) {
            _send_serial_unlocked(base, s[off + i]);
        }

#ifdef _KERNEL
        spin_unlock_irqrestore(&serial.tx_lock, flags);
#endif

        off += chunk;
    }
}
