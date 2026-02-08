#include <base/attributes.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/serial.h>

static void _serial_puts(const char* s) {
    send_serial_string(SERIAL_COM1, s);
}

NORETURN void _kern_entry(boot_info_t* info) {
    (void)info;

    init_serial(SERIAL_COM1, SERAIL_DEFAULT_LINE, SERIAL_DEFAULT_BAUD);

#if defined(__x86_64__)
    _serial_puts("apheleiaOS kernel (x86_64) booted\r\n");
#else
    _serial_puts("apheleiaOS kernel (x86_32) booted\r\n");
#endif

    halt();
}
