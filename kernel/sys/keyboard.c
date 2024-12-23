#include "keyboard.h"

#include <base/attributes.h>
#include <ctype.h>
#include <data/ring.h>

#include "input/kbd.h"
#include "input/keymap.h"
#include "log/log.h"
#include "sys/tty.h"
#include "vfs/fs.h"


// TODO: more than one keyboards?
static bool shifted = false;
static bool capslocked = false;
static ascii_keymap* keymap = &us_keymap;

static ring_buffer* buffer;


static isize _read(UNUSED vfs_node* node, void* buf, UNUSED usize offset, usize len) {
    if (!buf)
        return -1;

    bool popped = ring_buffer_pop_array(buffer, buf, len);

    return popped ? len : 0;
}


void kbd_handle_key(key_event event) {
    u8 code = event.code;

    if (code == KBD_LEFT_SHIFT || code == KBD_RIGHT_SHIFT)
        shifted = is_down(event);

    if ((code == KBD_CAPSLOCK) && is_down(event))
        capslocked = !capslocked;

    // No new key press to register
    if (!is_down(event))
        return;

    char ch = kbd_to_ascii(event, *keymap, shifted);

    if (!ch)
        return;

    if (capslocked) {
        if (shifted)
            ch = tolower(ch);
        else
            ch = toupper(ch);
    }

    // Push to the device buffer
    ring_buffer_push(buffer, ch);

    // Send the ascii key to the current virtual tty
    tty_current_input(ch);
}


bool keyboard_init(void) {
    vfs_node* dev = vfs_create_node("kbd", VFS_CHARDEV);
    dev->interface = vfs_create_file_interface(_read, NULL);

    vfs_mount("/dev", tree_create_node(dev));

    buffer = ring_buffer_create(KBD_DEV_BUFFER_SIZE);

    log_debug("Initialised the keyboard");

    return true;
}
