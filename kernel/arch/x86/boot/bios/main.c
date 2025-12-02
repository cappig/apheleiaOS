#include <base/attributes.h>
#include <base/types.h>
#include <x86/lib/asm.h>
#include <x86/lib/boot.h>
#include <x86/lib/regs.h>
#include <x86/lib/serial.h>

#include "config.h"
#include "disk.h"
#include "elf.h"
#include "memory.h"
#include "stdlib.h"
#include "tty.h"
#include "vesa.h"

ALIGNED(8)
static boot_info_t info = {0};


NORETURN
void _load_entry(u16 boot_disk) {
    init_serial(SERIAL_COM1, SERAIL_DEFAULT_LINE, SERIAL_DEFAULT_BAUD);

    printf("Booting apheleiaOS...\n\r");

    get_e820(&info.memory_map);
    init_malloc();

    get_rsdp(&info.acpi_root_ptr);

    disk_init(boot_disk);

    parse_config(&info.args);

    // init_graphics(&info);

    puts("Jumping to kernel...\n\r");

    load_kerenel(&info);

    halt();
    __builtin_unreachable();
}
