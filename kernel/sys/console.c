#include "console.h"

#include <base/types.h>
#include <data/ring.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <term/term.h>
#include <x86/serial.h>

#include "sys/tty.h"

static ring_buffer* console_buffer = NULL;

static usize com_port = SERIAL_COM1;
static isize console_tty = TTY_NONE;


void kputsn(const char* str, usize len) {
    if (console_buffer)
        ring_buffer_push_array(console_buffer, (u8*)str, len);

    if (com_port)
        send_serial_sized_string(com_port, str, len);

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


void conosle_init_buffer() {
    console_buffer = ring_buffer_create(CONSOLE_BUF_SIZE);
}

void console_set_serial(usize port) {
    com_port = port;
}

void console_set_tty(usize index) {
    console_tty = index;
}
