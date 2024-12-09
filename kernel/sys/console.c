#include "console.h"

#include <base/types.h>
#include <data/ring.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <term/term.h>
#include <x86/serial.h>

#include "sys/tty.h"
#include "term/render.h"


static ring_buffer* console_buffer = NULL;

static usize com_port = SERIAL_COM1;


// Dump the entire buffer to the ccurrent terminal
// We use this when the kernel panics
void console_dump_buffer() {
    if (!current_tty)
        return;

    u8 ch;
    while (ring_buffer_pop(console_buffer, &ch))
        term_parse_char(current_tty->term, ch);
}


void kputsn(const char* str, usize len) {
    if (com_port)
        send_serial_string(com_port, str);

    if (console_buffer)
        ring_buffer_push_array(console_buffer, (u8*)str, len);
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
