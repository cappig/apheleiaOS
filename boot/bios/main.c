#include <base/attributes.h>
#include <base/types.h>
#include <boot/proto.h>
#include <x86/asm.h>
#include <x86/serial.h>

#include "e820.h"
#include "tty.h"
#include "vesa.h"

static boot_handoff handoff = {.magic = BOOT_MAGIC};


NORETURN void _load_entry(u16 boot_disk) {
    init_serial(SERIAL_DEFAULT_BAUD);

    get_e820(&handoff.mmap);

    init_graphics(&handoff.graphics, GFX_VESA, 1280, 720, 32);

    panic("Well shit");

    halt();
    __builtin_unreachable();
}
