#include <base/attributes.h>
#include <base/types.h>
#include <x86/asm.h>
#include <x86/serial.h>

#include "tty.h"


NORETURN void _load_entry(u16 boot_disk) {
    init_serial(SERIAL_DEFAULT_BAUD);

    panic("Well shit");

    halt();
    __builtin_unreachable();
}
