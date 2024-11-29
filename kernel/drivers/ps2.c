#include "ps2.h"

#include <base/types.h>
#include <data/ring.h>
#include <log/log.h>
#include <x86/asm.h>

#include "arch/idt.h"
#include "arch/irq.h"
#include "vfs/fs.h"

static ring_buffer* buffer;

// Zeroes represent non ASCII chars
// TODO: don't just hardcode this
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

static void ps2_irq_handler(UNUSED int_state* s) {
    u8 scancode = inb(0x60);

    // TODO: don't just push raw scancodes. Parse into a more common format
    ring_buffer_push(buffer, scancode);

#ifdef PS2_DEBUG
    log_debug("[PS2 DEBUG] scancode = %#x, ascii = %c", scancode, ps2_to_ascii(scancode));
#endif

    irq_ack(IRQ_PS2_KEYBOARD);
}

static isize _read(UNUSED vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    if (!buf)
        return -1;

    ring_buffer_pop_array(buffer, buf, len);

    return len;
}

static isize _write(UNUSED vfs_node* node, UNUSED void* buf, UNUSED usize offset, UNUSED usize len) {
    return -1;
}


char ps2_to_ascii(u8 scancode) {
    static bool shift_down = false;

    if (scancode == KBD_L_SHIFT || scancode == KBD_R_SHIFT) {
        shift_down = true;
        return 0;
    } else if (scancode == PS2_RELEASED(KBD_L_SHIFT) || scancode == PS2_RELEASED(KBD_R_SHIFT)) {
        shift_down = false;
        return 0;
    }

    return shift_down ? us_ascii[1][scancode] : us_ascii[0][scancode];
}

void init_ps2_kbd(virtual_fs* vfs) {
    irq_register(IRQ_PS2_KEYBOARD, ps2_irq_handler);

    vfs_node* dev = vfs_create_node("kbd", VFS_CHARDEV);
    dev->interface = vfs_create_file_interface(_read, _write);

    buffer = ring_buffer_create(PS2_DEV_BUFFER_SIZE);

    vfs_mount(vfs, "/dev", tree_create_node(dev));
}
