#include "ps2.h"

#include <base/types.h>
#include <x86/asm.h>

#include "arch/idt.h"
#include "log/log.h"
#include "vfs/fs.h"
#include "vfs/pipe.h"

static vfs_node* chardev;

// Zeroes represent non ASCII chars
static const u8 us_ascii[2][256] = {
    {
        0,   0,   '1', '2', '3', '4', '5', '6', '7',  '8', '9', '0',  '-',  '=', 0,   '\t',
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o',  'p', '[', ']',  '\n', 0,   'a', 's',
        'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z',  'x', 'c', 'v',
        'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,    ' ', 0,   0,    0,    0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   '-', 0,    0,    0,   '+',
    },
    {
        0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',  '+', 0,   '\t',
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
        'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z',  'X', 'C', 'V',
        'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,    0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '-', 0,   0,    0,   '+',
    },
};


static char _get_ascii(u8 scancode) {
    static bool shift_down = false;

    if (scancode == KBD_L_SHIFT || scancode == KBD_R_SHIFT) {
        shift_down = true;
        return 0;
    } else if (scancode == RELEASED(KBD_L_SHIFT) || scancode == RELEASED(KBD_R_SHIFT)) {
        shift_down = false;
        return 0;
    }

    return shift_down ? us_ascii[1][scancode] : us_ascii[0][scancode];
}

static void ps2_irq_handler(UNUSED int_state* s) {
    u8 scancode = inb(0x60);

    // FIXME: don't just push raw scancodes. Parse into a more common format
    chardev->interface->file.write(chardev, (u8[]){scancode}, 0, 1);

    log_debug("PS2 scancode: %#x ascii: %c", scancode, _get_ascii(scancode));
}

void init_ps2_kbd(virtual_fs* vfs) {
    set_int_handler(IRQ_NUMBER(IRQ_PS2_KEYBOARD), ps2_irq_handler);

    // TODO: number this?
    chardev = pipe_create("kbd", 128);
    vfs_mount(vfs, "/dev", chardev);
}
