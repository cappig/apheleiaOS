#include <arch/arch.h>
#include <base/types.h>
#include <stddef.h>
#include <x86/serial.h>

ssize_t arch_console_read(void* buf, size_t len) {
    if (!buf)
        return -1;

    u8* out = buf;

    for (size_t i = 0; i < len; i++)
        out[i] = (u8)receive_serial(SERIAL_COM1);

    return (ssize_t)len;
}

ssize_t arch_console_write(const void* buf, size_t len) {
    if (!buf)
        return -1;

    send_serial_sized_string(SERIAL_COM1, buf, len);
    return (ssize_t)len;
}

ssize_t arch_tty_read(void* buf, size_t len) {
    return arch_console_read(buf, len);
}

ssize_t arch_tty_write(const void* buf, size_t len) {
    return arch_console_write(buf, len);
}
