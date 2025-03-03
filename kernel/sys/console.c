#include "console.h"

#include <base/types.h>
#include <boot/proto.h>
#include <data/ring.h>
#include <log/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <term/term.h>
#include <x86/serial.h>

#include "drivers/acpi.h"
#include "drivers/pci.h"
#include "drivers/serial.h"
#include "mem/physical.h"
#include "sys/tty.h"

static ring_buffer* console_buffer = NULL;

static usize com_port = 1;
static isize console_tty = TTY_NONE;


void kputsn(const char* str, usize len) {
    if (console_buffer)
        ring_buffer_push_array(console_buffer, (u8*)str, len);

    if (com_port)
        serial_output(com_port, (u8*)str, len);

    if (console_tty > 0)
        tty_output(console_tty, (u8*)str, len);
}

void kputs(const char* str) {
    kputsn(str, strlen(str));
}

int kprintf(char* fmt, ...) {
    char buf[KPRINTF_BUF_SIZE];

    va_list args;
    va_start(args, fmt);

    int ret = vsprintf(buf, fmt, args);

    kputs(buf);

    va_end(args);

    return ret;
}


void conosle_init(usize tty_index) {
    virtual_tty* vtty = get_tty(tty_index);

    // Turn off echoing
    vtty->pty->termios.c_lflag = ICANON | IEXTEN;

    console_tty = tty_index;

    console_buffer = ring_buffer_create(CONSOLE_BUF_SIZE);
}


void print_motd(boot_handoff* handoff) {
    log_info(ALPHA_ASCII);
    log_info(BUILD_DATE);

    if (handoff->args.debug == DEBUG_ALL) {
        dump_map(&handoff->mmap);
        dump_acpi_tables();
        dump_pci_devices();
    }

    if (handoff->args.debug >= DEBUG_MINIMAL)
        dump_mem();
}
