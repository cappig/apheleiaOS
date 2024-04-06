#include <base/attributes.h>
#include <base/types.h>
#include <x86/asm.h>

#include "tty.h"


NORETURN void _load_entry(u16 boot_disk) {
    panic("Well shit");

    halt();
    __builtin_unreachable();
}
